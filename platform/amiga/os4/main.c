/*
 * main.c - AmigaOS 4 handler entry wrapper
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "handler.h"

int main(void)
{
    handler_main();
    return 0;
}
