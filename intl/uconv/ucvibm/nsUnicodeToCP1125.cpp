/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsUCConstructors.h"
#include "nsUnicodeToCP1125.h"

//----------------------------------------------------------------------
// Global functions and data [declaration]

nsresult
nsUnicodeToCP1125Constructor(nsISupports *aOuter, REFNSIID aIID,
                            void **aResult) 
{
  static const uint16_t g_ufMappingTable[] = {
#include "cp1125.uf"
  };

  return CreateTableEncoder(u1ByteCharset, 
                            (uMappingTable*) &g_ufMappingTable, 1,
                            aOuter, aIID, aResult);
}

