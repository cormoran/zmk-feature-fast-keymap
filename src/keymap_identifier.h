/*
 * Copyright (c) 2026 cormoran
 * SPDX-License-Identifier: MIT
 */

#pragma once

#define FK_KEYMAP_IDENTIFIER_LENGTH 4

const char *fk_keymap_identifier_get(void);
int fk_keymap_identifier_set(const char *identifier);
