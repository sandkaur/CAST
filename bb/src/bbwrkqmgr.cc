/*******************************************************************************
 |    bbwrkqmgr.cc
 |
 |  � Copyright IBM Corporation 2015,2016. All Rights Reserved
 |
 |    This program is licensed under the terms of the Eclipse Public License
 |    v1.0 as published by the Eclipse Foundation and available at
 |    http://www.eclipse.org/legal/epl-v10.html
 |
 |    U.S. Government Users Restricted Rights:  Use, duplication or disclosure
 |    restricted by GSA ADP Schedule Contract with IBM Corp.
 *******************************************************************************/

#include <boost/filesystem.hpp>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <string.h>

#include "time.h"

#include "bbinternal.h"
#include "bbserver_flightlog.h"
#include "bbwrkqmgr.h"
#include "identity.h"
#include "BBTagInfo2.h"
#include "BBTagInfoMap2.h"
#include "Uuid.h"

namespace bfs = boost::filesystem;

FL_SetName(FLError, "Errordata flightlog")
FL_SetSize(FLError, 16384)


/*
 * Static data
 */


/*
 * Static methods
 */
string HeartbeatEntry::getHeartbeatCurrentTime()
{
	char l_Buffer[20] = {'\0'};
	char l_ReturnTime[27] = {'\0'};

    timeval l_CurrentTime;
    gettimeofday(&l_CurrentTime, NULL);
    unsigned long l_Micro = l_CurrentTime.tv_usec;

    // localtime is not thread safe
    // NOTE:  No spaces in the formatted timestamp, because this can be sent as part of an
    //        async request message.  That processing uses scanf to parse the data...
    strftime(l_Buffer, sizeof(l_Buffer), "%Y-%m-%d_%H:%M:%S", localtime((const time_t*)&l_CurrentTime.tv_sec));
    snprintf(l_ReturnTime, sizeof(l_ReturnTime), "%s.%06lu", l_Buffer, l_Micro);

    return string(l_ReturnTime);
}


/*
 * Non-static methods
 */

void WRKQMGR::addHPWorkItem(LVKey* pLVKey, BBTagID& pTagId)
{
    // Build the high priority work item
    WorkID l_WorkId(*pLVKey, pTagId);

    // Push the work item onto the HP work queue and post
    l_WorkId.dump("debug", "addHPWorkItem() ");
    HPWrkQE->addWorkItem(l_WorkId, DO_NOT_VALIDATE_WORK_QUEUE);
    WRKQMGR::post();

    return;
}

int WRKQMGR::addWrkQ(const LVKey* pLVKey, const uint64_t pJobId)
{
    int rc = 0;

    wrkqmgr.dump("debug", " - addWrkQ() before", DUMP_ALWAYS);

    std::map<LVKey,WRKQE*>::iterator it = wrkqs.find(*pLVKey);
    if (it == wrkqs.end())
    {
        WRKQE* l_WrkQE = new WRKQE(pLVKey, pJobId);
        l_WrkQE->setDumpOnRemoveWorkItem(config.get("bb.bbserverDumpWorkQueueOnRemoveWorkItem", DEFAULT_DUMP_QUEUE_ON_REMOVE_WORK_ITEM));
        wrkqs.insert(std::pair<LVKey,WRKQE*>(*pLVKey, l_WrkQE));
    }
    else
    {
        rc = -1;
        stringstream errorText;
        errorText << "Failure when trying to add workqueue for " << *pLVKey;
        LOG_ERROR_TEXT_RC(errorText, rc);
        wrkqmgr.dump("info", " Failure when trying to add workqueue", DUMP_ALWAYS);
    }

    wrkqmgr.dump("debug", " - addWrkQ() after", DUMP_ALWAYS);

    return rc;
}

int WRKQMGR::appendAsyncRequest(AsyncRequest& pRequest)
{
    int rc = 0;

    LOG(bb,debug) << "appendAsyncRequest(): Attempting an append to the async request file...";

    uid_t l_Uid = getuid();
    gid_t l_Gid = getgid();

  	becomeUser(0, 0);

    int l_SeqNbr = 0;
    FILE* fd = openAsyncRequestFile("ab", l_SeqNbr, MINIMAL_MAINTENANCE);
    if (fd != NULL)
    {
        char l_Buffer[sizeof(AsyncRequest)+1] = {'\0'};
        pRequest.str(l_Buffer, sizeof(l_Buffer));
        size_t l_Size = fwrite(l_Buffer, sizeof(char), sizeof(AsyncRequest), fd);
        fclose(fd);

        if (l_Size != sizeof(AsyncRequest))
        {
            rc = -1;
        }
    }
    else
    {
        rc = -1;
    }

    // Reset the heartbeat timer count...
    heartbeatTimerCount = 0;

  	becomeUser(l_Uid, l_Gid);

    if (!rc)
    {
        if (strstr(pRequest.data, "heartbeat"))
        {
            LOG(bb,debug) << "appendAsyncRequest(): Host name " << pRequest.hostname << " => " << pRequest.data;
        }
        else
        {
            LOG(bb,info) << "appendAsyncRequest(): Host name " << pRequest.hostname << " => " << pRequest.data;
        }
    }
    else
    {
        LOG(bb,error) << "appendAsyncRequest(): Could not append to the async request file. Failing append was: Host name " << pRequest.hostname << " => " << pRequest.data;
    }

    return rc;
}

void WRKQMGR::calcThrottleMode()
{
    int l_NewThrottleMode = 0;
    for (map<LVKey,WRKQE*>::iterator qe = wrkqs.begin(); qe != wrkqs.end(); qe++)
    {
        if (qe->second->getRate())
        {
            l_NewThrottleMode = 1;
            break;
        }
    }

    if (throttleMode != l_NewThrottleMode)
    {
        if (l_NewThrottleMode)
        {
            Throttle_Timer.forcePop();
            loadBuckets();
        }

        throttleMode = l_NewThrottleMode;
    }

    return;
}

uint64_t WRKQMGR::checkForNewHPWorkItems()
{
    uint64_t l_CurrentNumber = HPWrkQE->getNumberOfWorkItems();
    uint64_t l_NumberAdded = 0;

    int l_AsyncRequestFileSeqNbr = 0;
    int64_t l_OffsetToNextAsyncRequest = 0;

    int rc = findOffsetToNextAsyncRequest(l_AsyncRequestFileSeqNbr, l_OffsetToNextAsyncRequest);
    if (!rc)
    {
        if (l_AsyncRequestFileSeqNbr > 0 && l_OffsetToNextAsyncRequest >= 0)
        {
            bool l_FirstFile = true;
            int l_CurrentAsyncRequestFileSeqNbr = 0;
            uint64_t l_CurrentOffsetToNextAsyncRequest = 0;
            uint64_t l_TargetOffsetToNextAsyncRequest = 0;
            getOffsetToNextAsyncRequest(l_CurrentAsyncRequestFileSeqNbr, l_CurrentOffsetToNextAsyncRequest);
            while (l_CurrentAsyncRequestFileSeqNbr <= l_AsyncRequestFileSeqNbr)
            {
                // NOTE: If we set the offset target to MAXIMUM_ASYNC_REQUEST_FILE_SIZE, we have to add 1 to it so that we process the last async request
                //       in that file.  Otherwise, we don't add 1 because that is the offset to the next async request to be inserted.
                l_TargetOffsetToNextAsyncRequest = (l_CurrentAsyncRequestFileSeqNbr < l_AsyncRequestFileSeqNbr ? MAXIMUM_ASYNC_REQUEST_FILE_SIZE+1 : (uint64_t)l_OffsetToNextAsyncRequest);
                if (!l_FirstFile)
                {
                    // We crossed the boundary to a new async request file...  Start at offset zero...
                    l_CurrentOffsetToNextAsyncRequest = 0;
                    LOG(bb,info) << "Starting to process async requests from async request file sequence number " << l_CurrentAsyncRequestFileSeqNbr;
                }
                while (l_CurrentOffsetToNextAsyncRequest < l_TargetOffsetToNextAsyncRequest)
                {
                    // Enqueue the data items to the high priority work queue
                    const string l_ConnectionName = "None";
                    LVKey l_LVKey = std::pair<string, Uuid>(l_ConnectionName, Uuid(HP_UUID));
                    size_t l_Offset = l_CurrentOffsetToNextAsyncRequest;
                    BBTagID l_TagId(BBJob(), l_Offset);

                    // Build/push the work item onto the HP work queue and post
                    addHPWorkItem(&l_LVKey, l_TagId);
                    l_CurrentOffsetToNextAsyncRequest += sizeof(AsyncRequest);
                    ++l_NumberAdded;
                }
                l_FirstFile = false;
                ++l_CurrentAsyncRequestFileSeqNbr;
            }

            if (l_NumberAdded)
            {
                // NOTE:  Have to decrement the request file sequence number as it will always be incremented past the last file
                //        sequence number that was processed...
                wrkqmgr.setOffsetToNextAsyncRequest(--l_CurrentAsyncRequestFileSeqNbr, (uint64_t)l_CurrentOffsetToNextAsyncRequest);
                LOG(bb,debug) << "checkForNewHPWorkItems(): Found " << l_NumberAdded << " new async requests";
            }
        }
        else
        {
            LOG(bb,error) << "Error occured when attempting to read the cross bbserver async request file, l_AsyncRequestFileSeqNbr = " \
                          << l_AsyncRequestFileSeqNbr << ", l_OffsetToNextAsyncRequest = " << l_OffsetToNextAsyncRequest;
        }
    }
    else
    {
        LOG(bb,error) << "Error occured when attempting to read the cross bbserver async request file, rc = " << rc;
    }

    return l_CurrentNumber + l_NumberAdded;
}

void WRKQMGR::checkThrottleTimer()
{
    if (Throttle_Timer.popped(Throttle_TimeInterval))
    {
        LOG(bb,off) << "WRKQMGR::checkThrottleTimer(): Popped";

        // Check/add new high priority work items from the cross bbserver metadata
        checkForNewHPWorkItems();

        // See if it is time to dump the work manager
        if (dumpTimerPoppedCount && (++dumpTimerCount >= dumpTimerPoppedCount))
        {
            dump("info", " Work Queue Mgr (Timer Interval)", DUMP_ALWAYS);
        }

        // See if it is time to reload the work queue throttle buckets
        if (++throttleTimerCount >= throttleTimerPoppedCount)
        {
            // If any workqueue in throttle mode, load the buckets
            if (inThrottleMode())
            {
                loadBuckets();
            }
            throttleTimerCount = 0;
            setDelayMessageSent(false);
        }

        // See if it is time to have a heartbeat for this bbServer
        if (++heartbeatTimerCount >= heartbeatTimerPoppedCount)
        {
            // Tell the world this bbServer is still alive...
            char l_AsyncCmd[AsyncRequest::MAX_DATA_LENGTH] = {'\0'};
            string l_CurrentTime = HeartbeatEntry::getHeartbeatCurrentTime();
            snprintf(l_AsyncCmd, sizeof(l_AsyncCmd), "heartbeat 0 0 0 0 0 None %s", l_CurrentTime.c_str());
            AsyncRequest l_Request = AsyncRequest(l_AsyncCmd);
            appendAsyncRequest(l_Request);
        }

        // See if it is time to dump the heartbeat information
        if (heartbeatDumpPoppedCount && (++heartbeatDumpCount >= heartbeatDumpPoppedCount))
        {
            dumpHeartbeatData("info");
        }
    }

    return;
}

int WRKQMGR::createAsyncRequestFile(const char* pAsyncRequestFileName)
{
    int rc = 0;

    char l_Cmd[PATH_MAX+64] = {'\0'};
    snprintf(l_Cmd, sizeof(l_Cmd), "touch %s 2>&1;", pAsyncRequestFileName);

    for (auto&l_Line : runCommand(l_Cmd)) {
        // No expected output...
        LOG(bb,error) << l_Line;
        rc = -1;
    }

    if (!rc)
    {
        // NOTE:  This could be slightly misleading.  We have a race condition between multiple
        //        bbServers possibly attempting to create the same named async request file.
        //        But, touch is being used.  So if the file were to be created just before this bbServer
        //        attempts to create it, all should still be OK with just the last reference dates
        //        being updated.  However, both bbServers will claim to have created the file.
        LOG(bb,info) << "WRKQMGR: New async request file " << pAsyncRequestFileName << " created";
    }

    return rc;
}

void WRKQMGR::dump(const char* pSev, const char* pPostfix, DUMP_OPTION pDumpOption) {
    const char* l_PostfixOverride = " (Number of Skipped Dumps Exceeded)";
    char* l_PostfixStr = const_cast<char*>(pPostfix);

    if (allowDump)
    {
        if (pDumpOption==DUMP_ALWAYS || inThrottleMode())
        {
            bool l_DumpIt = false;
            if (numberOfWorkQueueItemsProcessed != lastDumpedNumberOfWorkQueueItemsProcessed)
            {
                l_DumpIt = true;
            }
            else
            {
                if (numberOfAllowedSkippedDumpRequests && numberOfSkippedDumpRequests > numberOfAllowedSkippedDumpRequests)
                {
                    l_DumpIt = true;
                    l_PostfixStr = const_cast<char*>(l_PostfixOverride);
                }
            }
            if (l_DumpIt)
            {
                if (!strcmp(pSev,"debug")) {
                    LOG(bb,debug) << ">>>>> Start: WRKQMGR" << l_PostfixStr << " <<<<<";
//                    LOG(bb,debug) << "                 Throttle Mode: " << (throttleMode ? "true" : "false") << "  TransferQueue Locked: " << (transferQueueLocked ? "true" : "false");
                    LOG(bb,debug) << "                 Throttle Mode: " << (throttleMode ? "true" : "false") << "  Number of Workqueue Items Processed: " << numberOfWorkQueueItemsProcessed \
                                  << "  Check Canceled Extents: " << (checkForCanceledExtents ? "true" : "false") << "  Snoozing: " << (Throttle_Timer.isSnoozing() ? "true" : "false");
//                    LOG(bb,debug) << "          Throttle Timer Count: " << throttleTimerCount << "  Throttle Timer Popped Count: " << throttleTimerPoppedCount;
//                    LOG(bb,debug) << "         Heartbeat Timer Count: " << dumpTimerCount << " Heartbeat Timer Popped Count: " << dumpTimerPoppedCount;
//                    LOG(bb,debug) << "          Heartbeat Dump Count: " << heartbeatDumpCount << "  Heartbeat Dump Popped Count: " << heartbeatDumpPoppedCount;
//                    LOG(bb,debug) << "              Dump Timer Count: " << heartbeatTimerCount << "          Dump Timer Popped Count: " << heartbeatTimerPoppedCount;
//                    LOG(bb,debug) << "     Declare Server Dead Count: " << declareServerDeadCount;
                    LOG(bb,debug) << "          Last Queue Processed: " << lastQueueProcessed << "  SeqNbr: " << asyncRequestFileSeqNbr << " Offset: 0x" \
                                  << hex << uppercase << setfill('0') << setw(8) <<offsetToNextAsyncRequest << setfill(' ') << nouppercase << dec;
                    LOG(bb,debug) << "   Number of Workqueue Entries: " << wrkqs.size();
                    for (map<LVKey,WRKQE*>::iterator qe = wrkqs.begin(); qe != wrkqs.end(); qe++)
                    {
                        qe->second->dump(pSev, "          ");
                    }
                    LOG(bb,debug) << ">>>>>   End: WRKQMGR" << l_PostfixStr << " <<<<<";
                } else if (!strcmp(pSev,"info")) {
                    LOG(bb,info) << ">>>>> Start: WRKQMGR" << l_PostfixStr << " <<<<<";
//                    LOG(bb,info) << "                 Throttle Mode: " << (throttleMode ? "true" : "false") << "  TransferQueue Locked: " << (transferQueueLocked ? "true" : "false");
                    LOG(bb,info) << "                 Throttle Mode: " << (throttleMode ? "true" : "false") << "  Number of Workqueue Items Processed: " << numberOfWorkQueueItemsProcessed \
                                 << "  Check Canceled Extents: " << (checkForCanceledExtents ? "true" : "false") << "  Snoozing: " << (Throttle_Timer.isSnoozing() ? "true" : "false");
//                    LOG(bb,info) << "          Throttle Timer Count: " << throttleTimerCount << "  Throttle Timer Popped Count: " << throttleTimerPoppedCount;
//                    LOG(bb,info) << "         Heartbeat Timer Count: " << dumpTimerCount << " Heartbeat Timer Popped Count: " << dumpTimerPoppedCount;
//                    LOG(bb,info) << "          Heartbeat Dump Count: " << heartbeatDumpCount << "  Heartbeat Dump Popped Count: " << heartbeatDumpPoppedCount;
//                    LOG(bb,info) << "              Dump Timer Count: " << dumpTimerCount << "      Dump Timer Popped Count: " << dumpTimerPoppedCount;
//                    LOG(bb,info) << "     Declare Server Dead Count: " << declareServerDeadCount;
                    LOG(bb,info) << "          Last Queue Processed: " << lastQueueProcessed << "  SeqNbr: " << asyncRequestFileSeqNbr << "  Offset: 0x" \
                                 << hex << uppercase << setfill('0') << setw(8) << offsetToNextAsyncRequest << setfill(' ') << nouppercase << dec;
                    LOG(bb,info) << "   Number of Workqueue Entries: " << wrkqs.size();
                    for (map<LVKey,WRKQE*>::iterator qe = wrkqs.begin(); qe != wrkqs.end(); qe++)
                    {
                        qe->second->dump(pSev, "          ");
                    }
                    LOG(bb,info) << ">>>>>   End: WRKQMGR" << l_PostfixStr << " <<<<<";
                }
                lastDumpedNumberOfWorkQueueItemsProcessed = numberOfWorkQueueItemsProcessed;
                numberOfSkippedDumpRequests = 0;

                // NOTE:  Only reset the dumpTimerCount if we actually dumped the work queue manager.
                //        Then, if no transfer activity for a while, we will dump the work queue manager
                //        the next time the throttle timer pops.
                dumpTimerCount = 0;
            }
            else
            {
                // Nothing has changed...  Skip the dump...
                ++numberOfSkippedDumpRequests;
            }
        }
    }

    return;
}

void WRKQMGR::dump(queue<WorkID>* l_WrkQ, WRKQE* l_WrkQE, const char* pSev, const char* pPrefix)
{
    char l_Temp[64] = {'\0'};
    if (wrkqs.size() == 1)
    {
        strCpy(l_Temp, " queue exists: ", sizeof(l_Temp));
    }
    else
    {
        strCpy(l_Temp, " queues exist: ", sizeof(l_Temp));
    }

    l_WrkQE->dump(pSev, pPrefix);

    return;
}

void WRKQMGR::dumpHeartbeatData(const char* pSev, const char* pPrefix)
{
    if (heartbeatData.size())
    {
        int i = 1;
        if (!strcmp(pSev,"debug"))
        {
            LOG(bb,debug) << ">>>>> Start: " << (pPrefix ? pPrefix : "") << heartbeatData.size() \
                          << (heartbeatData.size()==1 ? " reporting bbServer <<<<<" : " reporting bbServers <<<<<");
            for (auto it=heartbeatData.begin(); it!=heartbeatData.end(); ++it)
            {
                LOG(bb,debug) << i << ") " << it->first << " -> Count " << (it->second).getCount() \
                              << ", time reported: " << (it->second).getTime() \
                              << ", timestamp from bbServer: " << (it->second).getServerTime();
                ++i;
            }
            LOG(bb,debug) << ">>>>>   End: " << heartbeatData.size() \
                          << (heartbeatData.size()==1 ? " reporting bbServer <<<<<" : " reporting bbServers <<<<<");
        }
        else if (!strcmp(pSev,"info"))
        {
            LOG(bb,info) << ">>>>> Start: " << (pPrefix ? pPrefix : "") << heartbeatData.size() \
                         << (heartbeatData.size()==1 ? " reporting bbServer <<<<<" : " reporting bbServers <<<<<");
            for (auto it=heartbeatData.begin(); it!=heartbeatData.end(); ++it)
            {
                LOG(bb,info) << i << ") " << it->first << " -> Count " << (it->second).getCount() \
                              << ", time reported: " << (it->second).getTime() \
                              << ", timestamp from bbServer: " << (it->second).getServerTime();
                ++i;
            }
            LOG(bb,info) << ">>>>>   End: " << heartbeatData.size() \
                         << (heartbeatData.size()==1 ? " reporting bbServer <<<<<" : " reporting bbServers <<<<<");
        }
    }
    else
    {
        if (!strcmp(pSev,"debug"))
        {
            LOG(bb,debug) << ">>>>>   No other reporting bbServers";
        }
        else if (!strcmp(pSev,"info"))
        {
            LOG(bb,info) << ">>>>>   No other reporting bbServers";
        }
    }

    heartbeatDumpCount = 0;

    return;
}

int WRKQMGR::findOffsetToNextAsyncRequest(int &pSeqNbr, int64_t &pOffset)
{
    int rc = 0;

    pSeqNbr = 0;
    pOffset = 0;

    uid_t l_Uid = getuid();
    gid_t l_Gid = getgid();

  	becomeUser(0, 0);

    FILE* fd = openAsyncRequestFile("rb", pSeqNbr);
    if (fd != NULL)
    {
        rc = fseek(fd, 0, SEEK_END);
        if (!rc)
        {
            pOffset = (int64_t)ftell(fd);
        }
        else
        {
            rc = -1;
        }
        fclose(fd);
    }
    else
    {
        rc = -1;
    }

  	becomeUser(l_Uid, l_Gid);

    return rc;
}

int WRKQMGR::findWork(const LVKey* pLVKey, WRKQE* &pWrkQE)
{
    int rc = 0;
    pWrkQE = 0;

    if (pLVKey)
    {
        rc = getWrkQE(pLVKey, pWrkQE);
    }
    else
    {
        // First, check the high priority work queue
        if (HPWrkQE->getWrkQ()->size())
        {
            // High priority work exists...  Pass the high priority queue back...
            pWrkQE = HPWrkQE;
        }
        else
        {
            if (getCheckForCanceledExtents())
            {
                // Search for any LVKey work queue with a canceled extent at the front...
                rc = getWrkQE_WithCanceledExtents(pWrkQE);
            }

            if (!rc)
            {
                if (!pWrkQE)
                {
                    // No work queue found with canceled extents.
                    // Find 'real' work on one of the LVKey work queues...
                    LVKey l_LVKey = std::pair<string, Uuid>("", Uuid());
                    rc = getWrkQE(&l_LVKey, pWrkQE);
                }
                else
                {
                    // At least one work queue exists with a canceled extent at the front.
                    // Return the work queue identified by pWrkQE.
                }
            }
            else
            {
                // \todo - Error case, but can't get here...
            }
        }
    }

    return rc;
}

int WRKQMGR::getAsyncRequest(WorkID& pWorkItem, AsyncRequest& pRequest)
{
    int rc = 0;

    char l_Buffer[sizeof(AsyncRequest)+1] = {'\0'};

    uid_t l_Uid = getuid();
    gid_t l_Gid = getgid();

  	becomeUser(0, 0);

    int l_SeqNbr = 0;
    FILE* fd = openAsyncRequestFile("rb", l_SeqNbr);
    if (fd != NULL)
    {
        fseek(fd, pWorkItem.getTag(), SEEK_SET);
        size_t l_Size = fread(l_Buffer, sizeof(char), sizeof(AsyncRequest), fd);
        fclose(fd);

        if (l_Size == sizeof(AsyncRequest))
        {
            pRequest = AsyncRequest(l_Buffer, l_Buffer+AsyncRequest::MAX_HOSTNAME_LENGTH);
        }
        else
        {
            rc = -1;
        }
    }
    else
    {
        rc = -1;
    }

  	becomeUser(l_Uid, l_Gid);

    return rc;
}

int WRKQMGR::getThrottleRate(LVKey* pLVKey, uint64_t& pRate)
{
    int rc = 0;
    pRate = 0;

    std::map<LVKey,WRKQE*>::iterator it = wrkqs.find(*pLVKey);
    if (it != wrkqs.end())
    {
        pRate = it->second->getRate();
    }
    else
    {
        // NOTE: This may be tolerated...  Set rc to -2
        rc = -2;
    }

    return rc;
}

int WRKQMGR::getWrkQE(const LVKey* pLVKey, WRKQE* &pWrkQE)
{
    int rc = 0;
    LVKey l_LVKey;
    WRKQE* l_WrkQE = 0;
    int64_t l_BucketValue = -1;
    bool l_Continue = true;

    pWrkQE = 0;

    // NOTE: In the 'normal' case, we many be looking for a non-existent work queue in either
    //       the null or non-null LVKey paths.
    //
    //       If a job is ended, the extents are removed from the appropriate work queue, but the counting
    //       semaphore is not reduced by the number of extents removed.  Therefore, we may be sent into
    //       this routine without any remaining work queue entries for a given work queue, for a workqueue that
    //       no longer exists, or where there are no existing work queues.
    //
    //       In these cases, pWorkQE is always returned as NULL.  If ANY work queue exists with at least
    //       one entry, rc is returned as zero.  If no entry exists on any work queue
    //       (except for the high priority work queue), rc is returned as -1.
    //
    //       A zero return code indicates that even if a work queue entry was not found, a repost should
    //       be performed to the semaphore.  A return code of -1 indicates that a repost is not necessary.
    //
    // NOTE: Suspended work queues are returned by this method.  The reason for this is that an entry can
    //       still be removed from a suspended work queue if the extent is for a canceled transfer definition.
    //       Thus, we must return suspended work queues from this method.

    if ((pLVKey->second).is_null())
    {
//        verify();
        // NOTE: The HP work queue is always present, so there must be at least two work queues...
        if (wrkqs.size() > 1)
        {
            // We have one or more workqueues...
            if (wrkqs.begin()->second != HPWrkQE)
            {
                // Not the HP workqueue...
                if (wrkqs.begin()->second->wrkq->size())
                {
                    // This workqueue has at least one entry...
                    //
                    // If we can't find a 'next' WRKQE to return,
                    // we will return this one unless we find a better one...
                    l_LVKey = wrkqs.begin()->first;
                    l_WrkQE = wrkqs.begin()->second;
                    l_BucketValue = wrkqs.begin()->second->getBucket();

                    // If the last queue processed is the last in the map and this queue (the first queue in the map)
                    // has a non-negative bucket value, return this queue now as it is the next in round robin order.
                    if (*(wrkqs.rbegin()->second->getLVKey()) == lastQueueProcessed && l_BucketValue >= 0)
                    {
                        l_Continue = false;
                    }
                }
            }

            if (l_Continue)
            {
                bool l_SelectNext = false;
                for (map<LVKey,WRKQE*>::iterator qe = wrkqs.begin(); qe != wrkqs.end(); qe++)
                {
                    bool l_Switch = false;
                    if ((qe->second != HPWrkQE) && (l_WrkQE != qe->second))
                    {
                        // Not the HP workqueue or the one we may have saved above...
                        if (qe->second->getWrkQ_Size())
                        {
                            // This workqueue has at least one entry...
                            if (l_WrkQE)
                            {
                                // We already have a workqueue to return.  Only switch to this
                                // workqueue if the current saved bucket value is not positive
                                // and this workqueue has a better 'bucket' value.
                                // NOTE: The 'switch' logic below is only for throtttling work queues.
                                //       Non-throttling work queues will simply be round-robined.
                                // NOTE: If all of the workqueues end up having a negative bucket
                                //       value, we return the workqueue with the least negative
                                //       bucket value.  This doesn't effect processing today,
                                //       but this should be kept in mind if the workqueue selection
                                //       algorithm is ever changed in the future.
                                // NOTE: If we end up delaying due to throttling, all threads
                                //       will be delaying on the same workqueue.  However, when the
                                //       delay time has expired, all of the threads will again return
                                //       to this routine to get the 'next' workqueue.
                                //       Returning to get the 'next' workqueue after a throttle delay
                                //       prevents a huge I/O spike for an individual workqueue.
                                if (l_BucketValue <= 0 && l_BucketValue < qe->second->getBucket())
                                {
                                    l_Switch = true;
                                }
                            }
                            else
                            {
                                // We don't have a workqueue to return.
                                // If we can't find a 'next' WRKQE to return,
                                // we will return this one unless we find a better one...
                                l_Switch = true;
                            }

                            if (l_Switch)
                            {
                                // This is a better workqueue to return if we
                                // can't find a 'next' WRKQE...
                                l_LVKey = qe->first;
                                l_WrkQE = qe->second;
                                l_BucketValue = qe->second->getBucket();
                            }

                            if (l_SelectNext)
                            {
                                // We are to return the 'next' workqueue that is not throttling -or- has a positive bucket value
                                if (qe->second->getRate() == 0 || qe->second->getBucket() > 0)
                                {
                                    // Non-throttling workqueue or positive bucket value (throttling).
                                    // Switch to this workqueue and return it...
                                    l_LVKey = qe->first;
                                    l_WrkQE = qe->second;
//                                    LOG(bb,info) << "WRKQMGR::getWrkQE(): l_SelectNextContinue = true, non-immediate next workqueue returned";
                                    break;
                                }
                            }
                        }
                    }

                    if ((!l_SelectNext) && qe->first == lastQueueProcessed)
                    {
                        // Just found the entry we last returned.  Return the next workqueue that has a positive bucket value...
//                        LOG(bb,info) << "WRKQMGR::getWrkQE(): l_SelectNext = true";
                        l_SelectNext = true;
                    }
                }

                if (l_WrkQE)
                {
                    // NOTE: We don't update the last queue processed here becasue our invoker may choose to not take action on our returned
                    //       data.  The last queue processed is updated just before an item of work is removed from a queue.
                    pWrkQE = l_WrkQE;
                }
                else
                {
                    // WRKQMGR::getWrkQE(): No extents left on any workqueue
                    rc = -1;
//                    LOG(bb,info) << "WRKQMGR::getWrkQE(): No extents left on any workqueue";
                }
            }
        }
        else
        {
            // WRKQMGR::getWrkQE(): No workqueue entries exist
            rc = -1;
//            LOG(bb,info) << "WRKQMGR::getWrkQE(): No workqueue entries exist";
        }
    }
    else
    {
        std::map<LVKey,WRKQE*>::iterator it = wrkqs.find(*pLVKey);
        if (it != wrkqs.end())
        {
            // NOTE: In this path since a specific LVKey was passed, we do not interrogate the
            //       rate/bucket values.  We simply return the work queue associated with the LVKey.
            pWrkQE = it->second;
        }
        else
        {
            // WRKQMGR::getWrkQE(): Workqueue no longer exists
            //
            // NOTE: rc is returned as a zero in this case.  We do not know if
            //       any other workqueue has an entry...
            LOG(bb,info) << "WRKQMGR::getWrkQE(): Workqueue for " << *pLVKey << " no longer exists";
            dump("info", " Work Queue Mgr (Specific workqueue not found)", DUMP_ALWAYS);
        }
    }

    return rc;
}

int WRKQMGR::getWrkQE_WithCanceledExtents(WRKQE* &pWrkQE)
{
    int rc = 0;
    LVKey l_Key;
    BBTagInfo2* l_TagInfo2 = 0;

    pWrkQE = 0;
    // NOTE: The HP work queue is always present, so there must be at least two work queues.
    //       We only need to return a work queue if there are at least two LVKey work queues.
    //       This is because we only need to differentiate between a queue with no canceled
    //       extents and one with canceled extents.  The normal findWork() processing can
    //       do the necessary processing if there are only two work queues.
    if (wrkqs.size() > 2)
    {
        for (map<LVKey,WRKQE*>::iterator qe = wrkqs.begin(); qe != wrkqs.end(); qe++)
        {
            // For each of the work queues...
            if (qe->second != HPWrkQE)
            {
                // Not the HP workqueue...
                if (qe->second->wrkq->size())
                {
                    // This workqueue has at least one entry.
                    // Get the LVKey and taginfo2 for this work item...
                    l_Key = (qe->second->getWrkQ()->front()).getLVKey();
                    l_TagInfo2 = metadata.getTagInfo2(&l_Key);
                    if (l_TagInfo2 && ((l_TagInfo2->getNextExtentInfo().getTransferDef()->canceled())))
                    {
                        // Next extent is canceled...  Don't look any further
                        // and simply return this work queue.
                        pWrkQE = qe->second;
                        break;
                    }
                }
            }
        }
    }

    if (!pWrkQE)
    {
        // Indicate that we no longer need to look for canceled extents
        setCheckForCanceledExtents(0);
    }

    return rc;
}

void WRKQMGR::loadBuckets()
{
    for (map<LVKey,WRKQE*>::iterator qe = wrkqs.begin(); qe != wrkqs.end(); qe++)
    {
        if (qe->second != HPWrkQE)
        {
            qe->second->loadBucket();
        }
    }

    return;
}

// NOTE: pLVKey is not currently used, but can come in as null.
void WRKQMGR::lock(const LVKey* pLVKey, const char* pMethod)
{
    if (!transferQueueIsLocked())
    {
        pthread_mutex_lock(&lock_transferqueue);
        if (strstr(pMethod, "%") == NULL)
        {
            if (pLVKey)
            {
                LOG(bb,debug) << "TRNFR_Q:   LOCK <- " << pMethod << ", " << *pLVKey;
            }
            else
            {
                LOG(bb,debug) << "TRNFR_Q:   LOCK <- " << pMethod << ", unknown LVKey";
            }
        }
        transferQueueLocked = pthread_self();

        pid_t tid = syscall(SYS_gettid);  // \todo eventually remove this.  incurs syscall for each log entry
        FL_Write(FLMutex, lockTransferQ, "lockTransfer.  threadid=%ld",tid,0,0,0);
    }
    else
    {
        if (lockPinned)
        {
            // The lock is already owned, but the lock is pinned...  So, all OK...
            LOG(bb,debug) << "TRNFR_Q: Request made to lock the transfer queue by " << pMethod << ", but the lock is already pinned.";
        }
        else
        {
            FL_Write(FLError, lockTransferQERROR, "lockTransferQueue called when lock already owned by thread",0,0,0,0);
            flightlog_Backtrace(__LINE__);
            // For now, also to the console...
            LOG(bb,error) << "TRNFR_Q: Request made to lock the transfer queue by " << pMethod << ", but the lock is already owned.";
            logBacktrace();
        }
    }

    return;
}

FILE* WRKQMGR::openAsyncRequestFile(const char* pOpenOption, int &pSeqNbr, const int pMaintenanceOption)
{
    FILE* l_FilePtr = 0;
    char* l_AsyncRequestFileNamePtr = 0;
    pSeqNbr = 0;

    int rc = verifyAsyncRequestFile(l_AsyncRequestFileNamePtr, pSeqNbr, pMaintenanceOption);
    if ((!rc) && l_AsyncRequestFileNamePtr)
    {
        bool l_AllDone = false;
        while (!l_AllDone)
        {
            l_AllDone = true;
            l_FilePtr = fopen(l_AsyncRequestFileNamePtr, pOpenOption);
            if (l_FilePtr != NULL && pOpenOption[0] == 'a')
            {
                // Append mode...  Check the file size...
                uint64_t l_Offset = (int64_t)ftell(l_FilePtr);
                if (l_Offset > MAXIMUM_ASYNC_REQUEST_FILE_SIZE)
                {
                    // Time for a new async request file...
                    delete [] l_AsyncRequestFileNamePtr;
                    l_AsyncRequestFileNamePtr = 0;
                    rc = verifyAsyncRequestFile(l_AsyncRequestFileNamePtr, pSeqNbr, CREATE_NEW_FILE);
                    if (!rc)
                    {
                        // Close the file...
                        fclose(l_FilePtr);
                        l_FilePtr = 0;

                        // Iterate to open the new file...
                        l_AllDone = false;
                    }
                    else
                    {
                        // Error case...  Just return this file...
                        // \todo - Code error case @DLH
                    }
                }
            }
        }
    }

    if (l_AsyncRequestFileNamePtr)
    {
        delete [] l_AsyncRequestFileNamePtr;
        l_AsyncRequestFileNamePtr = 0;
    }

    return l_FilePtr;
}

// NOTE: pLVKey is not currently used, but can come in as null.
void WRKQMGR::pinLock(const LVKey* pLVKey, const char* pMethod)
{
    if(transferQueueIsLocked())
    {
        if (!lockPinned)
        {
            lockPinned = 1;
            if (strstr(pMethod, "%") == NULL)
            {
                if (pLVKey)
                {
                    LOG(bb,info) << "TRNFR_Q:   LOCK PINNED <- " << pMethod << ", " << *pLVKey;
                }
                else
                {
                    LOG(bb,info) << "TRNFR_Q:   LOCK PINNED <- " << pMethod << ", unknown LVKey";
                }
            }
        }
        else
        {
            LOG(bb,error) << "TRNFR_Q:   Request made to pin the transfer queue lock by " << pMethod << ", " << *pLVKey << ", but the pin is already in place.";
        }
    }
    else
    {
        LOG(bb,error) << "TRNFR_Q:   Request made to pin the transfer queue lock by " << pMethod << ", " << *pLVKey << ", but the lock is not held.";
    }

    return;
}

void WRKQMGR::processAllOutstandingHP_Requests(const LVKey* pLVKey)
{
    // NOTE: We currently hold the lock transfer queue lock.  Therefore, we essentially process all of the
    //       outstanding async requests in FIFO order and even if bbServer is multi-threaded, we serialize
    //       the processing for each of these requests.  This is true even if the processing for an individual
    //       request releases and re-acquires the lock as part of its processing.
    uint32_t i = 0;
    bool l_AllDone= false;

    // First, check for any new appended HP work queue items...
    uint64_t l_NumberToProcess = checkForNewHPWorkItems();

    // Now, process all enqueued high priority work items...
    // NOTE: \todo - Is it possible for so many async requests to be appended (and continue to be appended...)
    //               that we never process them all...  Seems unlikely...  Investigate...  @DLH
    while (!l_AllDone)
    {
        // NOTE: Currently set to log after 1 second of not being able to process all async requests, and every 10 seconds thereafter...
        if ((i % 20) == 2)
        {
            LOG(bb,info) << "processAllOutstandingHP_Requests():  HPWrkQE->getNumberOfWorkItemsProcessed() = " << HPWrkQE->getNumberOfWorkItemsProcessed() << ", l_NumberToProcess = " << l_NumberToProcess;
        }
        if (HPWrkQE->getNumberOfWorkItemsProcessed() >= l_NumberToProcess)
        {
            l_AllDone = true;
        }
        else
        {
            unlockTransferQueue(pLVKey, "processAllOutstandingHP_Requests");
            {
                // NOTE: Currently set to log after 1 second of not being able to process all async requests, and every 10 seconds thereafter...
                if ((i++ % 20) == 2)
                {
                    LOG(bb,info) << ">>>>> DELAY <<<<< processAllOutstandingHP_Requests(): Processing all outstanding async requests. Delay of 500 milliseconds.";
                }
                usleep((useconds_t)500000);
            }
            lockTransferQueue(pLVKey, "processAllOutstandingHP_Requests");
        }
    }

    return;
}

void WRKQMGR::processThrottle(LVKey* pLVKey, BBTagInfo2* pTagInfo2, BBTagID& pTagId, ExtentInfo& pExtentInfo, Extent* pExtent, double& pThreadDelay, double& pTotalDelay)
{
    WRKQE* l_WrkQE = 0;
    pThreadDelay = 0;
    pTotalDelay = 0;

    // Check to see if the throttle timer has popped.
    // If so, any new high priority work items are pushed onto the
    // high priority work queue from the cross bbserver metadata.
    // If we are in throttle mode, the buckets are also (re)loaded.
    checkThrottleTimer();

    if (inThrottleMode())
    {
        if (!getWrkQE(pLVKey, l_WrkQE))
        {
            if(l_WrkQE)
            {
                pThreadDelay = l_WrkQE->processBucket(pTagInfo2, pTagId, pExtentInfo);
                pTotalDelay = (pThreadDelay ? ((double)(throttleTimerPoppedCount-throttleTimerCount-1) * (Throttle_TimeInterval*1000000)) + pThreadDelay : 0);
            }
        }
    }

    return;
}

void WRKQMGR::removeWorkItem(WRKQE* pWrkQE, WorkID& pWorkItem)
{
    if (pWrkQE)
    {
        // Perform any dump operations...
        if (pWrkQE->getDumpOnRemoveWorkItem())
        {
            // NOTE: Only dump out the found work if for the high priority work queue -or-
            //       this is the last entry in the queue -or- there this a multiple of the number of entries interval
            queue<WorkID>* l_WrkQ = pWrkQE->getWrkQ();
            if ((l_WrkQ == HPWrkQE->getWrkQ()) || (pWrkQE->getWrkQ_Size() == 1) ||
                (getDumpOnRemoveWorkItemInterval() && getNumberOfWorkQueueItemsProcessed() % getDumpOnRemoveWorkItemInterval() == 0))
            {
                pWrkQE->dump("info", "Start: Current work item -> ");
                if (dumpOnRemoveWorkItem && (l_WrkQ != HPWrkQE->getWrkQ()))
                {
                    dump("info", " Work Queue Mgr (Count Interval)", DUMP_ALWAYS);
                }
            }
            else
            {
                pWrkQE->dump("debug", "Start: Current work item -> ");
                if (dumpOnRemoveWorkItem && (l_WrkQ != HPWrkQE->getWrkQ()))
                {
                    dump("debug", " Work Queue Mgr (Debug)", DUMP_ALWAYS);
                }
            }
        }

        // Remove the work item from the work queue
        pWrkQE->removeWorkItem(pWorkItem, VALIDATE_WORK_QUEUE);

        // Update the last processed work queue in the manager
        setLastQueueProcessed(pWrkQE->getLVKey());

        // Increment number of work items processed
        incrementNumberOfWorkItemsProcessed();
    }
    else
    {
        // \todo - Set error text...
    }

    return;
}

int WRKQMGR::rmvWrkQ(const LVKey* pLVKey)
{
    int rc = 0;

    wrkqmgr.dump("debug", " - rmvWrkQ() before", DUMP_ALWAYS);

    std::map<LVKey,WRKQE*>::iterator it = wrkqs.find(*pLVKey);
    if (it != wrkqs.end())
    {
        WRKQE* l_WrkQE = it->second;
        wrkqs.erase(it);
        if (l_WrkQE)
        {
            delete l_WrkQE;
        }
    }
    else
    {
        LOG(bb,error) << "Failure when trying to remove a workqueue";
        rc = -1;
    }

    wrkqmgr.dump("debug", " - rmvWrkQ() after", DUMP_ALWAYS);

    return rc;
}

void WRKQMGR::setDumpTimerPoppedCount(const double pTimerInterval)
{
    double l_DumpTimeInterval = config.get("bb.bbserverDumpWorkQueueMgr_TimeInterval", DEFAULT_DUMP_MGR_TIME_INTERVAL);
    dumpTimerPoppedCount = (int64_t)(l_DumpTimeInterval/pTimerInterval);
    if (((double)dumpTimerPoppedCount)*pTimerInterval != (double)(l_DumpTimeInterval))
    {
        ++dumpTimerPoppedCount;
        LOG(bb,warning) << "Dump timer interval of " << to_string(l_DumpTimeInterval) << " second(s) is not a common multiple of " << pTimerInterval << " second(s).  Any dumpping rates may be implemented as slightly less than what is specified.";
    }

    return;
}

void WRKQMGR::setHeartbeatDumpPoppedCount(const double pTimerInterval)
{
    double l_HeartbeatDumpInterval = config.get("bb.bbserverHeartbeat_DumpInterval", DEFAULT_BBSERVER_HEARTBEAT_DUMP_INTERVAL);
    heartbeatDumpPoppedCount = (int64_t)(l_HeartbeatDumpInterval/pTimerInterval);
    if (((double)heartbeatDumpPoppedCount)*pTimerInterval != (double)(l_HeartbeatDumpInterval))
    {
        ++heartbeatDumpPoppedCount;
        LOG(bb,warning) << "Dump timer interval of " << to_string(l_HeartbeatDumpInterval) << " second(s) is not a common multiple of " << pTimerInterval << " second(s).  Any heartbeat dump rates may be implemented as slightly less than what is specified.";
    }

    return;
}

void WRKQMGR::setHeartbeatTimerPoppedCount(const double pTimerInterval)
{
    double l_HeartbeatTimeInterval = config.get("bb.bbserverHeartbeat_TimeInterval", DEFAULT_BBSERVER_HEARTBEAT_TIME_INTERVAL);
    heartbeatTimerPoppedCount = (int64_t)(l_HeartbeatTimeInterval/pTimerInterval);
    if (((double)heartbeatTimerPoppedCount)*pTimerInterval != (double)(l_HeartbeatTimeInterval))
    {
        ++heartbeatTimerPoppedCount;
        LOG(bb,warning) << "Dump timer interval of " << to_string(l_HeartbeatTimeInterval) << " second(s) is not a common multiple of " << pTimerInterval << " second(s).  Any heartbeat rates may be implemented as slightly less than what is specified.";
    }

    // Currently, for a restart transfer operation, we will wait a total
    // of twice the bbServer heartbeat before declaring a bbServer dead
    // because the transfer definiiton is not marked as being stopped.
    declareServerDeadCount = heartbeatTimerPoppedCount * 2;

    return;
}

int WRKQMGR::setSuspended(const LVKey* pLVKey, const int pValue)
{
    int rc = 0;

    std::map<LVKey,WRKQE*>::iterator it = wrkqs.find(*pLVKey);
    if (it != wrkqs.end())
    {
        if ((pValue && (!it->second->isSuspended())) || ((!pValue) && it->second->isSuspended()))
        {
            it->second->setSuspended(pValue);
        }
        else
        {
            rc = 2;
        }
    }
    else
    {
        rc = -2;
    }

    return rc;
}


int WRKQMGR::setThrottleRate(const LVKey* pLVKey, const uint64_t pRate)
{
    int rc = 0;

    std::map<LVKey,WRKQE*>::iterator it = wrkqs.find(*pLVKey);
    if (it != wrkqs.end())
    {
        it->second->setRate(pRate);
        calcThrottleMode();
    }
    else
    {
        rc = -2;
    }

    return rc;
}

void WRKQMGR::setThrottleTimerPoppedCount(const double pTimerInterval)
{
    throttleTimerPoppedCount = (int)(1.0/pTimerInterval);
    if (((double)throttleTimerPoppedCount)*pTimerInterval != (double)1.0)
    {
        ++throttleTimerPoppedCount;
        LOG(bb,warning) << "Throttle timer interval of " << pTimerInterval << " second is not a common multiple of 1.0 second.  Any throttling rates may be implemented as slightly less than what is specified.";
    }

    return;
}

// NOTE: pLVKey is not currently used, but can come in as null.
void WRKQMGR::unlock(const LVKey* pLVKey, const char* pMethod)
{
    if (transferQueueIsLocked())
    {
        if (!lockPinned)
        {
            pid_t tid = syscall(SYS_gettid);  // \todo eventually remove this.  incurs syscall for each log entry
            FL_Write(FLMutex, unlockTransferQ, "unlockTransfer.  threadid=%ld",tid,0,0,0);

            transferQueueLocked = 0;
            if (strstr(pMethod, "%") == NULL)
            {
                if (pLVKey)
                {
                    LOG(bb,debug) << "TRNFR_Q: UNLOCK <- " << pMethod << ", " << *pLVKey;
                }
                else
                {
                    LOG(bb,debug) << "TRNFR_Q: UNLOCK <- " << pMethod << ", unknown LVKey";
                }
            }
            pthread_mutex_unlock(&lock_transferqueue);
        }
        else
        {
            LOG(bb,debug) << "TRNFR_Q: Request made to unlock the transfer queue by " << pMethod << ", but the lock is pinned.";
        }
    }
    else
    {
        FL_Write(FLError, unlockTransferQERROR, "unlockTransferQueue called when lock not owned by thread",0,0,0,0);
        flightlog_Backtrace(__LINE__);
        // For now, also to the console...
        LOG(bb,error) << "TRNFR_Q: Request made to unlock the transfer queue by " << pMethod << ", but the lock is not owned.";
        logBacktrace();
    }

    return;
}

// NOTE: pLVKey is not currently used, but can come in as null.
void WRKQMGR::unpinLock(const LVKey* pLVKey, const char* pMethod)
{
    if(transferQueueIsLocked())
    {
        if (lockPinned)
        {
            lockPinned = 0;
            if (strstr(pMethod, "%") == NULL)
            {
                if (pLVKey)
                {
                    LOG(bb,info) << "TRNFR_Q:   LOCK UNPINNED <- " << pMethod << ", " << *pLVKey;
                }
                else
                {
                    LOG(bb,info) << "TRNFR_Q:   LOCK UNPINNED <- " << pMethod << ", unknown LVKey";
                }
            }
        }
        else
        {
            LOG(bb,error) << "TRNFR_Q:   Request made to unpin the transfer queue lock by " << pMethod << ", " << *pLVKey << ", but the pin is not in place.";
        }
    }
    else
    {
        LOG(bb,error) << "TRNFR_Q:   Request made to unpin the transfer queue lock by " << pMethod << ", " << *pLVKey << ", but the lock is not held.";
    }

    return;
}

// NOTE: Stageout End processing can 'discard' extents from a work queue.  Therefore, the number of
//       posts can exceed the current number of extents.  Such posts will eventually be consumed by the worker
//       threads and treated as no-ops.  Note that the stageout end processing cannot re-initialize
//       the semaphore to the 'correct' value as you cannot re-init a counting semaphore...  @DLH
void WRKQMGR::verify()
{
    int l_TotalExtents = getSizeOfAllWorkQueues();

    int l_NumberOfPosts = 0;
    sem_getvalue(&sem_workqueue, &l_NumberOfPosts);

    // NOTE: l_NumberOfPosts+1 because for us to be invoking verify(), the current thread has already been dispatched...
    if (l_NumberOfPosts+1 != l_TotalExtents)
    {
        LOG(bb,info) << "WRKQMGR::verify(): MISMATCH: l_NumberOfPosts=" << l_NumberOfPosts << ", l_TotalExtents=" << l_TotalExtents;
        dump(const_cast<char*>("info"), " - After failed verification");
    }

    return;
}

void WRKQMGR::updateHeartbeatData(const string& pHostName)
{
    uint64_t l_Count = 0;

    string l_CurrentTime = HeartbeatEntry::getHeartbeatCurrentTime();
    map<string, HeartbeatEntry>::iterator it = heartbeatData.find(pHostName);
    if (it != heartbeatData.end())
    {
        l_Count = (it->second).getCount();
        heartbeatData[pHostName] = HeartbeatEntry(++l_Count, l_CurrentTime, (it->second).getServerTime());
    }
    else
    {
        heartbeatData[pHostName] = HeartbeatEntry(++l_Count, l_CurrentTime, "");
    }

    return;
}

void WRKQMGR::updateHeartbeatData(const string& pHostName, const string& pServerTimeStamp)
{
    uint64_t l_Count = 0;

    map<string, HeartbeatEntry>::iterator it = heartbeatData.find(pHostName);
    if (it != heartbeatData.end())
    {
        l_Count = (it->second).getCount();
    }

    string l_CurrentTime = HeartbeatEntry::getHeartbeatCurrentTime();
    HeartbeatEntry l_Entry = HeartbeatEntry(++l_Count, l_CurrentTime, pServerTimeStamp);

    heartbeatData[pHostName] = l_Entry;

    return;
}

int WRKQMGR::verifyAsyncRequestFile(char* &pAsyncRequestFileName, int &pSeqNbr, const int pMaintenanceOption)
{
    int rc = 0;
    stringstream errorText;

    int l_SeqNbr = 0;
    int l_CurrentSeqNbr = 0;

    pAsyncRequestFileName = new char[PATH_MAX+1];
    string l_DataStorePath = config.get("bb.bbserverMetadataPath", DEFAULT_BBSERVER_METADATAPATH);

    bfs::path datastore(l_DataStorePath);
    if(bfs::is_directory(datastore))
    {
        for(auto& asyncfile : boost::make_iterator_range(bfs::directory_iterator(datastore), {}))
        {
            if(bfs::is_directory(asyncfile)) continue;

            int l_Count = sscanf(asyncfile.path().filename().c_str(),"asyncRequests_%d", &l_CurrentSeqNbr);
            if (l_Count == 1 && l_CurrentSeqNbr > l_SeqNbr)
            {
                l_SeqNbr = l_CurrentSeqNbr;
                strCpy(pAsyncRequestFileName, asyncfile.path().c_str(), PATH_MAX+1);
            }
        }
    }
    else
    {
        bfs::create_directories(datastore);
        LOG(bb,info) << "WRKQMGR: Directory " << datastore << " created to house the cross bbServer metadata";
    }

    if (!l_SeqNbr)
    {
        l_SeqNbr = 1;
        snprintf(pAsyncRequestFileName, PATH_MAX+1, "%s/%s_%d", l_DataStorePath.c_str(), XBBSERVER_ASYNC_REQUEST_BASE_FILENAME.c_str(), l_SeqNbr);
        rc = createAsyncRequestFile(pAsyncRequestFileName);
        if (rc)
        {
            errorText << "Failure when attempting to create new cross bbserver async request file";
            bberror << err("error.filename", pAsyncRequestFileName);
            LOG_ERROR_TEXT_RC_AND_BAIL(errorText, rc);
        }
    }

    if (!rc)
    {
        try
        {
            switch (pMaintenanceOption)
            {
                case CREATE_NEW_FILE:
                {
                    l_SeqNbr += 1;
                    snprintf(pAsyncRequestFileName, PATH_MAX+1, "%s/%s_%d", l_DataStorePath.c_str(), XBBSERVER_ASYNC_REQUEST_BASE_FILENAME.c_str(), l_SeqNbr);
                    rc = createAsyncRequestFile(pAsyncRequestFileName);
                    if (rc)
                    {
                        errorText << "Failure when attempting to create new cross bbserver async request file";
                        bberror << err("error.filename", pAsyncRequestFileName);
                        LOG_ERROR_TEXT_RC_AND_BAIL(errorText, rc);
                    }
                }
                // Fall through...

                case FULL_MAINTENANCE:
                {
                    // Unconditionally perform a chown to root:root for the cross-bbserver metatdata root directory.
                    rc = chown(l_DataStorePath.c_str(), 0, 0);
                    if (rc)
                    {
                        errorText << "chown failed";
                        bberror << err("error.path", l_DataStorePath.c_str());
                        LOG_ERROR_TEXT_ERRNO_AND_BAIL(errorText, rc);
                    }

                    // Unconditionally perform a chmod to 0777 for the cross-bbserver metatdata root directory.
                    // NOTE:  Users for all jobs must be able to insert into this directory.
                    rc = chmod(l_DataStorePath.c_str(), 0777);
                    if (rc)
                    {
                        errorText << "chmod failed";
                        bberror << err("error.path", l_DataStorePath.c_str());
                        LOG_ERROR_TEXT_ERRNO_AND_BAIL(errorText, rc);
                    }

                    // Unconditionally perform a chown to root:root for the async request file.
                    rc = chown(pAsyncRequestFileName, 0, 0);
                    if (rc)
                    {
                        errorText << "chown failed";
                        bberror << err("error.path", pAsyncRequestFileName);
                        LOG_ERROR_TEXT_ERRNO_AND_BAIL(errorText, rc);
                    }

                    // Unconditionally perform a chmod to 0700 for the async request file.
                    // NOTE:  root is the only user of the async request file.
                    rc = chmod(pAsyncRequestFileName, 0700);
                    if (rc)
                    {
                        errorText << "chmod failed";
                        bberror << err("error.path", pAsyncRequestFileName);
                        LOG_ERROR_TEXT_ERRNO_AND_BAIL(errorText, rc);
                    }

                    // Log where this instance of bbServer will start processing async requests
                    int l_AsyncRequestFileSeqNbr = 0;
                    int64_t l_OffsetToNextAsyncRequest = 0;
                    rc = wrkqmgr.findOffsetToNextAsyncRequest(l_AsyncRequestFileSeqNbr, l_OffsetToNextAsyncRequest);
                    if (!rc)
                    {
                        if (l_AsyncRequestFileSeqNbr > 0 && l_OffsetToNextAsyncRequest >= 0)
                        {
                            wrkqmgr.setOffsetToNextAsyncRequest(l_AsyncRequestFileSeqNbr, (uint64_t)l_OffsetToNextAsyncRequest);
                            LOG(bb,info) << "WRKQMGR: Current async request file = " << pAsyncRequestFileName << ", offsetToNextAsyncRequest = " << hex << uppercase << setfill('0') << setw(8) \
                                         << l_OffsetToNextAsyncRequest << setfill(' ') << nouppercase << dec;
                        }
                        else
                        {
                            rc = -1;
                            errorText << "Failure when attempting to open the cross bbserver async request file, l_AsyncRequestFileSeqNbr = " \
                                      << l_AsyncRequestFileSeqNbr << ", l_OffsetToNextAsyncRequest = " << l_OffsetToNextAsyncRequest;
                            LOG_ERROR_TEXT_RC_AND_BAIL(errorText, rc);
                        }
                    }
                    else
                    {
                        rc = -1;
                        errorText << "Failure when attempting to open the cross bbserver async request file, rc = " << rc;
                        LOG_ERROR_TEXT_RC_AND_BAIL(errorText, rc);
                    }
                }
                // Fall through...

                case MINIMAL_MAINTENANCE:
                {
                    bfs::path datastore(l_DataStorePath);
                    if(bfs::is_directory(datastore))
                    {
                        for(auto& asyncfile : boost::make_iterator_range(bfs::directory_iterator(datastore), {}))
                        {
                            if(bfs::is_directory(asyncfile)) continue;

                            if (asyncfile.path().filename().string() == XBBSERVER_ASYNC_REQUEST_BASE_FILENAME)
                            {
                                // Old style....  Simply delete it...
                                bfs::remove(asyncfile.path());
                                LOG(bb,info) << "WRKQMGR: Deprecated async request file " << asyncfile.path().c_str() << " removed";
                                continue;
                            }

                            int l_Count = sscanf(asyncfile.path().filename().c_str(),"asyncRequests_%d", &l_CurrentSeqNbr);
                            if (l_Count == 1 && l_CurrentSeqNbr < l_SeqNbr)
                            {
                                try
                                {
                                    // Old async file....  If old enough, delete it...
                                    struct stat l_Statinfo;
                                    int rc2 = stat(asyncfile.path().c_str(), &l_Statinfo);
                                    if (!rc2)
                                    {
                                        time_t l_CurrentTime = time(0);
                                        if (difftime(l_CurrentTime, l_Statinfo.st_atime) > ASYNC_REQUEST_FILE_PRUNE_TIME)
                                        {
                                            bfs::remove(asyncfile.path());
                                            LOG(bb,info) << "WRKQMGR: Async request file " << asyncfile.path() << " removed";
                                        }
                                    }
                                }
                                catch(exception& e)
                                {
                                    // NOTE:  It is possible for multiple bbServers to be performing maintenance at the same time.
                                    //        Tolerate any exception and continue...
                                }
                            }
                        }
                    }
                }
                // Fall through...

                case NO_MAINTENANCE:
                default:
                    break;
            }
       }
       catch(ExceptionBailout& e) { }
       catch(exception& e)
       {
           rc = -1;
           LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
       }
    }

    if (rc)
    {
        if (pAsyncRequestFileName)
        {
            delete [] pAsyncRequestFileName;
            pAsyncRequestFileName = 0;
        }
    }
    else
    {
        pSeqNbr = l_SeqNbr;
    }

    return rc;
}
