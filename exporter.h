/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef EXPORTER_H
#define EXPORTER_H

#include <signal.h>

#define EXPORTER_DEFAULT_ADDRESS "0.0.0.0"
#define EXPORTER_DEFAULT_PORT 19315

/* run the Prometheus exporter until break_flag is set by the SIGINT handler.
 * version is reported by the ib_exporter_build_info metric. listen_address is
 * an IPv4 address in dotted-quad notation. wait_mask is the signal mask applied
 * while blocking in pselect(), so SIGINT is only deliverable at that point.
 * returns 0 on clean shutdown, -1 on error.
 */
extern int exporter_run_server(const char *version, const char *listen_address, long int port, int ethernet_flag, const sigset_t *wait_mask, const volatile sig_atomic_t *break_flag);

#endif /* EXPORTER_H */
