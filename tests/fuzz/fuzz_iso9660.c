/*
 * fuzz_iso9660.c — forced ISO9660 parser smoke target
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "fuzz_common.h"

int main(int argc, char **argv)
{
    return fuzz_main_paths(argc, argv, ODFS_BACKEND_ISO9660, "fuzz_iso9660");
}
