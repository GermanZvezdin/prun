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

#include "common.h"

namespace worker {

unsigned int SHMEM_BLOCK_SIZE = 512 * 1024;
unsigned int MAX_SCRIPT_SIZE = SHMEM_BLOCK_SIZE - 1;

unsigned short DEFAULT_PORT = 5555;
unsigned short DEFAULT_UDP_PORT = DEFAULT_PORT - 1;
unsigned short DEFAULT_MASTER_UDP_PORT = DEFAULT_PORT - 2;

char const SHMEM_NAME[] = "prexec_shmem";
char const FIFO_NAME[] = "/tmp/.prexec";
char const UDS_NAME[] = "/tmp/.prexec_uds";

char const NODE_SCRIPT_NAME_PY[] = "node/node.py";
char const NODE_SCRIPT_NAME_JAVA[] = "node/node.java";
char const NODE_SCRIPT_NAME_SHELL[] = "node/node.sh";
char const NODE_SCRIPT_NAME_RUBY[] = "node/node.rb";
char const NODE_SCRIPT_NAME_JS[] = "node/node.js";

} // namespace worker
