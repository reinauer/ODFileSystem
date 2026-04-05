/*
 * fuzz_hfs.c — forced HFS parser smoke target
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "fuzz_common.h"

int main(int argc, char **argv)
{
    return fuzz_main_paths(argc, argv, ODFS_BACKEND_HFS, "fuzz_hfs");
}
