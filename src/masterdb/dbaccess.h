/*
===========================================================================

This software is licensed under the Apache 2 license, quoted below.

Copyright (C) 2013 Andrey Budnik <budnik27@gmail.com>

Licensed under the Apache License, Version 2.0 (the "License"); you may not
use this file except in compliance with the License. You may obtain a copy of
the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
License for the specific language governing permissions and limitations under
the License.

===========================================================================
*/

#ifndef __DB_ACCESS_H
#define __DB_ACCESS_H

#include <string>
#include <stdint.h>


namespace masterdb {

struct IDAO // Data Access Object
{
    virtual bool Put( int64_t key, const std::string &value ) = 0;
    virtual bool Get( int64_t key, std::string &value ) = 0;
    virtual bool Delete( int64_t key ) = 0;
};

} // namespace masterdb

#endif
