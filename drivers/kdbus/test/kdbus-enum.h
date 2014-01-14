/*
 * Copyright (C) 2013 Kay Sievers
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */
#pragma once

#include "../kdbus.h"

const char *enum_CMD(long long id);
const char *enum_MSG(long long id);
const char *enum_MATCH(long long id);
const char *enum_PAYLOAD(long long id);
