
/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
*
* The contents of this file are subject to the Netscape Public
* License Version 1.1 (the "License"); you may not use this file
* except in compliance with the License. You may obtain a copy of
* the License at http://www.mozilla.org/NPL/
*
* Software distributed under the License is distributed on an "AS
* IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
* implied. See the License for the specific language governing
* rights and limitations under the License.
*
* The Original Code is the JavaScript 2 Prototype.
*
* The Initial Developer of the Original Code is Netscape
* Communications Corporation.   Portions created by Netscape are
* Copyright (C) 1998 Netscape Communications Corporation. All
* Rights Reserved.
*
* Contributor(s):
*
* Alternatively, the contents of this file may be used under the
* terms of the GNU Public License (the "GPL"), in which case the
* provisions of the GPL are applicable instead of those above.
* If you wish to allow use of your version of this file only
* under the terms of the GPL and not to allow others to use your
* version of this file under the NPL, indicate your decision by
* deleting the provisions above and replace them with the notice
* and other provisions required by the GPL.  If you do not delete
* the provisions above, a recipient may use your version of this
* file under either the NPL or the GPL.
*/

#ifdef _WIN32
 // Turn off warnings about identifiers too long in browser information
#pragma warning(disable: 4786)
#pragma warning(disable: 4711)
#pragma warning(disable: 4710)
#endif


#include <algorithm>
#include <assert.h>

#include "world.h"
#include "utilities.h"
#include "js2value.h"

#include <map>
#include <algorithm>

#include "reader.h"
#include "parser.h"
#include "js2engine.h"
#include "bytecodecontainer.h"
#include "js2metadata.h"


namespace JavaScript {
namespace MetaData {



    BytecodeContainer::~BytecodeContainer() 
    { 
	for (std::vector<Multiname *>::iterator i = mMultinameList.begin(), end = mMultinameList.end(); (i != end); i++)
	    delete *i;
    }

    size_t BytecodeContainer::getPosition(uint16 pc)
    {
        for (std::vector<MapEntry>::iterator i = pcMap.begin(), end = pcMap.end(); (i != end); i++)
            if (i->first >= pc)
                return i->second;
        return 0;
    }

}
}


