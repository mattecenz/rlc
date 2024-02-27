/*
Copyright 2024 Massimo Fioravanti

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	 http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <inttypes.h>
#include <stdio.h>

#include "rlc/runtime/Runtime.h"

void rl_append_to_string__int64_t_String(int64_t* toConvert, String* out)
{
	char buffer[256];
	sprintf(buffer, "%" PRId64, *toConvert);
	rl_m_append__String_strlit(out, buffer);
}

void rl_append_to_string__int8_t_String(int8_t* toConvert, String* out)
{
	char buffer[256];
	sprintf(buffer, "%" PRId8, *toConvert);
	rl_m_append__String_strlit(out, buffer);
}

void rl_append_to_string__double_String(double* toConvert, String* out)
{
	char buffer[256];
	sprintf(buffer, "%f", *toConvert);
	rl_m_append__String_strlit(out, buffer);
}

void rl_print_string__String(String* s)
{
	int8_t* start = 0;
	int64_t index = 0;
	rl_m_get__String_int64_t_r_int8_tRef(&start, s, &index);
	puts((char*) start);
	fflush(stdin);
}

void rl_print_string__strlit(char* s)
{
	puts(s);
	fflush(stdin);
}
