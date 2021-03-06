/*================================================================================
   
    csmd/src/inv/include/inv_ssd_inventory.h

  © Copyright IBM Corporation 2017. All Rights Reserved

    This program is licensed under the terms of the Eclipse Public License
    v1.0 as published by the Eclipse Foundation and available at
    http://www.eclipse.org/legal/epl-v10.html

    U.S. Government Users Restricted Rights:  Use, duplication or disclosure
    restricted by GSA ADP Schedule Contract with IBM Corp.
 
================================================================================*/
#ifndef __INV_SSD_INVENTORY_H
#define __INV_SSD_INVENTORY_H

#include <stdint.h>       // Provides int32_t

#include "csmi/src/inv/include/inv_types.h"

// Returns true if successful, false if an error occurred 
// If successful, ssd_count will contain the number of entries populated in the ssd_inventory array
bool GetSsdInventory(csm_ssd_inventory_t ssd_inventory[CSM_SSD_MAX_DEVICES], uint32_t& ssd_count);

#endif
