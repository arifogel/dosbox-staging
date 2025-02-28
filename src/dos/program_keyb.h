/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2021-2024  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef DOSBOX_PROGRAM_KEYB_H
#define DOSBOX_PROGRAM_KEYB_H

#include "programs.h"

#include "dos_keyboard_layout.h"

class KEYB final : public Program {
public:
	KEYB()
	{
		AddMessages();
		help_detail = {HELP_Filter::All,
		               HELP_Category::Misc,
		               HELP_CmdType::Program,
		               "KEYB"};
	}

	void Run() override;

private:
	void ListKeyboardLayouts();

	void WriteOutFailure(const KeyboardLayoutResult error_code,
	                     const std::string& layout,
	                     const uint16_t requested_code_page,
	                     const uint16_t tried_code_page);
	void WriteOutSuccess();

	static void AddMessages();
};

#endif // DOSBOX_PROGRAM_KEYB_H
