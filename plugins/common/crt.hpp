#ifndef __CRT_HPP__
#define __CRT_HPP__

#include "./CRT/delete.hpp"
#include "./CRT/delete_array.hpp"
#include "./CRT/free.hpp"
#include "./CRT/malloc.hpp"
#include "./CRT/new.hpp"
#include "./CRT/new_array.hpp"
#include "./CRT/realloc.hpp"

#if defined(__GNUC__)

#include "./CRT/ctype.hpp"
#include "./CRT/memchr.hpp"
#include "./CRT/memcmp.hpp"
#include "./CRT/memcpy.hpp"
#include "./CRT/memmove.hpp"
#include "./CRT/memset.hpp"
#include "./CRT/strchr.hpp"
#include "./CRT/strncmp.hpp"
#include "./CRT/strpbrk.hpp"
#include "./CRT/strrchr.hpp"
#include "./CRT/strstr.hpp"
#include "./CRT/strtok.hpp"

#endif

#endif
