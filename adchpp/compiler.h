/*
 * Copyright (C) 2006-2025 Jacek Sieka, arnetheduck on gmail point com
 * Copyright (C) 2026 iceman50
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#if defined(__clang__)
#if __clang_major__ < 3 || (__clang_major__ == 3 && __clang_minor__ < 3)
#error Clang 3.3 or newer is required for C++11 support
#endif

#elif defined(__GNUC__)
#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 9)
#error GCC 4.9 or newer is required for C++11 support
#endif

#elif defined(_MSC_VER)
#if _MSC_VER < 1900
#error MSVC 14 (Visual Studio 2015) or newer is required for C++11 support
#endif

#else
#error No supported compiler found

#endif

#if !defined(_MSC_VER) && __cplusplus < 201103L
#error ADCH++ must be compiled in C++11 mode or newer
#endif
