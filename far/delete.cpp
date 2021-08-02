﻿/*
delete.cpp

Удаление файлов
*/
/*
Copyright © 1996 Eugene Roshal
Copyright © 2000 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// BUGBUG
#include "platform.headers.hpp"

// Self:
#include "delete.hpp"

// Internal:
#include "flink.hpp"
#include "scantree.hpp"
#include "treelist.hpp"
#include "constitle.hpp"
#include "TPreRedrawFunc.hpp"
#include "taskbar.hpp"
#include "interf.hpp"
#include "keyboard.hpp"
#include "message.hpp"
#include "config.hpp"
#include "pathmix.hpp"
#include "dirmix.hpp"
#include "panelmix.hpp"
#include "mix.hpp"
#include "dirinfo.hpp"
#include "wakeful.hpp"
#include "stddlg.hpp"
#include "lang.hpp"
#include "FarDlgBuilder.hpp"
#include "strmix.hpp"
#include "uuids.far.dialogs.hpp"
#include "cvtname.hpp"
#include "fileattr.hpp"
#include "copy_progress.hpp"
#include "global.hpp"
#include "log.hpp"

// Platform:
#include "platform.fs.hpp"

// Common:
#include "common/scope_exit.hpp"
#include "common/view/enumerate.hpp"

// External:
#include "format.hpp"

//----------------------------------------------------------------------------

enum DEL_MODE
{
	DEL_SCAN,
	DEL_DEL,
	DEL_WIPE,
	DEL_WIPEPROCESS
};

static void PR_ShellDeleteMsg();

struct total_items
{
	size_t Items{};
	size_t Size{};
};

class ShellDelete : noncopyable
{
public:
	ShellDelete(panel_ptr SrcPanel, delete_type Type);

	struct progress
	{
		size_t Value;
		size_t Total;
	};

private:
	bool ConfirmDeleteReadOnlyFile(string_view Name, os::fs::attributes Attr);
	bool ShellRemoveFile(string_view Name, progress Files);
	bool ERemoveDirectory(string_view Name, delete_type Type, bool& RetryRecycleAsRemove);
	bool RemoveToRecycleBin(string_view Name, bool dir, bool& RetryRecycleAsRemove, bool& Skip);
	void process_item(
		panel_ptr SrcPanel,
		const os::fs::find_data& SelFindData,
		const total_items& Total,
		const time_check& TimeCheck,
		bool CannotRecycleTryRemove = false
	);

	int ReadOnlyDeleteMode{-1};
	int SkipWipeMode{-1};
	bool m_SkipFileErrors{};
	bool m_SkipFolderErrors{};
	bool m_DeleteFolders{};
	unsigned ProcessedItems{};
	bool m_UpdateDiz{};
	delete_type m_DeleteType;
};

struct DelPreRedrawItem : public PreRedrawItem
{
	DelPreRedrawItem():
		PreRedrawItem(PR_ShellDeleteMsg)
	{}

	string name;
	DEL_MODE Mode{};
	ShellDelete::progress Files{};
	int WipePercent{};
};

static void ShellDeleteMsgImpl(string_view const Name, DEL_MODE Mode, ShellDelete::progress Files, int WipePercent)
{
	string strProgress, strWipeProgress;
	const auto Width = copy_progress::CanvasWidth();

	if(Mode==DEL_WIPEPROCESS || Mode==DEL_WIPE)
	{
		strWipeProgress = make_progressbar(Width, WipePercent, true, !Files.Total);
	}

	if (Mode!=DEL_SCAN && Files.Total)
	{
		const auto Percent = ToPercent(Files.Value, Files.Total);
		strProgress = make_progressbar(Width, Percent, true, true);
		ConsoleTitle::SetFarTitle(concat(L'{', str(Percent), L"%} "sv, msg(Mode == DEL_WIPE || Mode == DEL_WIPEPROCESS? lng::MDeleteWipeTitle : lng::MDeleteTitle)));
	}

	{
		std::vector MsgItems
		{
			msg(Mode == DEL_SCAN ? lng::MScanningFolder : (Mode == DEL_WIPE || Mode == DEL_WIPEPROCESS) ? lng::MDeletingWiping : lng::MDeleting),
			fit_to_left(truncate_path(Name, Width), Width)
		};

		if (!strWipeProgress.empty())
			MsgItems.emplace_back(strWipeProgress);

		MsgItems.emplace_back(L"\x1"sv);
		MsgItems.emplace_back(copy_progress::FormatCounter(lng::MCopyFilesTotalInfo, lng::MCopyBytesTotalInfo, Files.Value, Files.Total, Files.Total != 0, copy_progress::CanvasWidth() - 5));

		if (!strProgress.empty())
			MsgItems.emplace_back(strProgress);

		Message(MSG_LEFTALIGN,
			msg((Mode == DEL_WIPE || Mode == DEL_WIPEPROCESS) ? lng::MDeleteWipeTitle : lng::MDeleteTitle),
			std::move(MsgItems),
			{});
	}
}

static void ShellDeleteMsg(string_view const Name, DEL_MODE Mode, ShellDelete::progress Files, int WipePercent)
{
	if (CheckForEscAndConfirmAbort())
		cancel_operation();

	ShellDeleteMsgImpl(Name, Mode, Files, WipePercent);

	TPreRedrawFunc::instance()([&](DelPreRedrawItem& Item)
	{
		Item.name = Name;
		Item.Mode = Mode;
		Item.Files = Files;
		Item.WipePercent = WipePercent;
	});
}

static void PR_ShellDeleteMsg()
{
	TPreRedrawFunc::instance()([](const DelPreRedrawItem& Item)
	{
		ShellDeleteMsgImpl(Item.name, Item.Mode, Item.Files, Item.WipePercent);
	});
}

static bool EraseFileData(string_view const Name, ShellDelete::progress Files)
{
	os::fs::file_walker File;
	if (!File.Open(Name, FILE_READ_DATA | FILE_WRITE_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN))
		return false;

	unsigned long long FileSize;
	if (!File.GetSize(FileSize))
		return false;

	if (!FileSize)
		return true; // nothing to do here

	const DWORD BufSize=65536;
	if (!File.InitWalk(BufSize))
		return false;

	const time_check TimeCheck(time_check::mode::immediate);

	std::mt19937 mt(clock()); // std::random_device doesn't work in w2k
	std::uniform_int_distribution CharDist(0, UCHAR_MAX);

	auto BufInit = false;

	do
	{
		static std::array<BYTE, BufSize> Buf;
		if (!BufInit)
		{
			if (Global->Opt->WipeSymbol == -1)
			{
				std::generate(ALL_RANGE(Buf), [&]{ return CharDist(mt); });
			}
			else
			{
				Buf.fill(Global->Opt->WipeSymbol);
				BufInit = true;
			}
		}

		if (!File.Write(Buf.data(), File.GetChunkSize()))
			return false;

		if (TimeCheck)
			ShellDeleteMsg(Name, DEL_WIPEPROCESS, Files, File.GetPercent());
	}
	while(File.Step());

	if (!File.SetPointer(0, nullptr, FILE_BEGIN))
		return false;

	if (!File.SetEnd())
		return false;

	return true;
}

static bool EraseFile(string_view const Name, ShellDelete::progress Files)
{
	if (!os::fs::set_file_attributes(Name, FILE_ATTRIBUTE_NORMAL))
		return false;

	if (!EraseFileData(Name, Files))
		return false;

	const auto strTempName = MakeTemp({}, false);

	if (!os::fs::move_file(Name, strTempName))
		return false;

	return os::fs::delete_file(strTempName);
}

static bool EraseDirectory(string_view const Name)
{
	auto Path = Name;

	if (!CutToParent(Path))
	{
		Path = {};
	}

	const auto strTempName = MakeTemp({}, false, Path);

	if (!os::fs::move_file(Name, strTempName))
	{
		return false;
	}

	return os::fs::remove_directory(strTempName);
}

static void show_confirmation(
	panel_ptr const SrcPanel,
	delete_type const DeleteType,
	size_t const SelCount,
	const os::fs::find_data& SingleSelData
)
{
	if (!Global->Opt->Confirm.Delete)
		return;

	lng TitleId, MessageId, ButtonId;
	const UUID* Id;

	if (DeleteType == delete_type::erase)
	{
		TitleId = lng::MDeleteWipeTitle;
		MessageId = lng::MAskWipe;
		ButtonId = lng::MDeleteWipe;
		Id = &DeleteWipeId;
	}
	else if (DeleteType == delete_type::remove)
	{
		TitleId = lng::MDeleteTitle;
		MessageId = lng::MAskDelete;
		ButtonId = lng::MDelete;
		Id = &DeleteFileFolderId;
	}
	else if (DeleteType == delete_type::recycle)
	{
		TitleId = lng::MDeleteTitle;
		MessageId = lng::MAskDeleteRecycle;
		ButtonId = lng::MDeleteRecycle;
		Id = &DeleteRecycleId;
	}
	else
		UNREACHABLE;

	std::vector<string> items;

	bool HighlightSelected = Global->Opt->DelOpt.HighlightSelected;

	if (SelCount == 1)
	{
		const auto IsFolder = (SingleSelData.Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		items.emplace_back(format(msg(MessageId), msg(IsFolder? lng::MAskDeleteFolder : lng::MAskDeleteFile)));
		items.emplace_back(QuoteOuterSpace(SingleSelData.FileName));

		if (SingleSelData.Attributes & FILE_ATTRIBUTE_REPARSE_POINT)
		{
			Id = &DeleteLinkId;

			auto FullName = ConvertNameToFull(SingleSelData.FileName);

			if (GetReparsePointInfo(FullName, FullName))
				NormalizeSymlinkName(FullName);

			items.emplace_back(msg(lng::MAskDeleteLink));
			items.emplace_back(std::move(FullName));
		}

		if (HighlightSelected)
		{
			string name, sname;
			if (SrcPanel->GetCurName(name, sname))
			{
				inplace::QuoteOuterSpace(name);
				HighlightSelected = SingleSelData.FileName != name;
			}
		}
	}
	else
	{
		items.emplace_back(format(msg(MessageId), msg(lng::MAskDeleteObjects)));

		const auto ItemsToShow = std::min(std::min(std::max(static_cast<size_t>(Global->Opt->DelOpt.ShowSelected), size_t{ 1 }), SelCount), size_t{ScrY / 2u});
		const auto ItemsMore = SelCount - ItemsToShow;

		for (const auto& i: SrcPanel->enum_selected())
		{
			items.emplace_back(QuoteOuterSpace(i.FileName));

			if (items.size() - 1 == ItemsToShow)
			{
				if (ItemsMore)
					items.emplace_back(format(msg(lng::MAskDeleteAndMore), ItemsMore));

				break;
			}
		}
	}

	intptr_t FirstHighlighted = 0, LastHighlighted = 0;

	DialogBuilder Builder(TitleId, {}, [&](Dialog* Dlg, intptr_t Msg, intptr_t Param1, void* Param2)
	{
		if (HighlightSelected && Msg == DN_CTLCOLORDLGITEM && in_closed_range(FirstHighlighted, Param1, LastHighlighted))
		{
			auto& Colors = *static_cast<const FarDialogItemColors*>(Param2);
			Colors.Colors[0] = Colors.Colors[1];
		}
		return Dlg->DefProc(Msg, Param1, Param2);
	});

	const auto MaxWidth = ScrX + 1 - 6 * 2;

	if (SelCount == 1)
	{
		for (const auto& [Item, Index]: enumerate(items))
		{
			inplace::truncate_center(Item, MaxWidth);
			Builder.AddText(Item)->Flags = DIF_CENTERTEXT | DIF_SHOWAMPERSAND;
			if (Index == 1)
				FirstHighlighted = LastHighlighted = Builder.GetLastID();
		}
	}
	else
	{
		for (const auto& [Item, Index]: enumerate(items))
		{
			if (Index == 1)
				Builder.AddSeparator();

			inplace::truncate_center(Item, MaxWidth);
			Builder.AddText(Item)->Flags = DIF_SHOWAMPERSAND;

			if (Index == 1)
				FirstHighlighted = Builder.GetLastID();
		}

		LastHighlighted = Builder.GetLastID();
	}

	Builder.AddOKCancel(ButtonId, lng::MCancel);
	Builder.SetId(*Id);

	if (DeleteType != delete_type::recycle)
		Builder.SetDialogMode(DMODE_WARNINGSTYLE);

	if (!Builder.ShowDialog())
		cancel_operation();
}

static total_items calculate_total(panel_ptr const SrcPanel)
{
	if (!Global->Opt->DelOpt.ShowTotal)
		return {};

	const time_check TimeCheck;
	total_items Total;
	dirinfo_progress const DirinfoProgress(msg(lng::MDeletingTitle));

	const auto DirInfoCallback = [&](string_view const Name, unsigned long long const ItemsCount, unsigned long long const Size)
	{
		if (!TimeCheck)
			return;

		DirinfoProgress.set_name(Name);
		DirinfoProgress.set_count(Total.Items + ItemsCount);
		DirinfoProgress.set_size(Total.Size + Size);
	};

	// BUGBUG
	for (const auto& i: SrcPanel->enum_selected())
	{
		++Total.Items;

		if (i.Attributes & FILE_ATTRIBUTE_DIRECTORY && !os::fs::is_directory_symbolic_link(i))
		{
			DirInfoData Data = {};

			if (GetDirInfo(i.FileName, Data, nullptr, DirInfoCallback, 0) <= 0)
				return {};

			Total.Items += Data.FileCount + Data.DirCount;
			Total.Size += Data.FileSize;
		}
	}

	return Total;
}

void ShellDelete::process_item(
	panel_ptr const SrcPanel,
	const os::fs::find_data& SelFindData,
	const total_items& Total,
	const time_check& TimeCheck,
	bool const CannotRecycleTryRemove
)
{
	const auto& strSelName = SelFindData.FileName;
	const auto& strSelShortName = SelFindData.AlternateFileName();

	if (strSelName.empty() || IsRelativeRoot(strSelName) || IsRootPath(strSelName))
		return;

	if (TimeCheck)
		ShellDeleteMsg(strSelName, m_DeleteType == delete_type::erase? DEL_WIPE : DEL_DEL, { ProcessedItems, Total.Items }, 0);

	if (!(SelFindData.Attributes & FILE_ATTRIBUTE_DIRECTORY))
	{
		if (ConfirmDeleteReadOnlyFile(strSelName, SelFindData.Attributes))
		{
			if (ShellRemoveFile(strSelName, { ProcessedItems, Total.Items }) && m_UpdateDiz)
				SrcPanel->DeleteDiz(strSelName, strSelShortName);
		}

		return;
	}

	const auto DirSymLink = os::fs::is_directory_symbolic_link(SelFindData);

	if (!m_DeleteFolders && !CannotRecycleTryRemove)
	{
		const auto strFullName = ConvertNameToFull(strSelName);

		if (os::fs::is_not_empty_directory(strFullName))
		{
			int MsgCode = 0; // для symlink не нужно подтверждение
			if (!DirSymLink)
			{
				auto Uuid = &DeleteFolderId;
				auto
					TitleId = lng::MDeleteFolderTitle,
					ConfirmId = lng::MDeleteFolderConfirm,
					DeleteId = lng::MDeleteFileDelete;

				if (m_DeleteType == delete_type::erase)
				{
					TitleId = lng::MWipeFolderTitle;
					ConfirmId = lng::MWipeFolderConfirm;
					DeleteId = lng::MDeleteFileWipe;
					Uuid = &WipeFolderId;
				}
				else if (m_DeleteType == delete_type::recycle)
				{
					ConfirmId = lng::MRecycleFolderConfirm;
					DeleteId = lng::MDeleteRecycle;
					Uuid = &DeleteFolderRecycleId;
				}

				MsgCode=Message(MSG_WARNING,
					msg(TitleId),
					{
						msg(ConfirmId),
						strFullName
					},
					{ DeleteId, lng::MDeleteFileAll, lng::MDeleteFileSkip, lng::MDeleteFileCancel },
					{}, Uuid);
			}

			if (MsgCode == Message::first_button)
			{
				// Nop
			}
			else if (MsgCode == Message::second_button)
			{
				m_DeleteFolders = true;
			}
			else if (MsgCode == Message::third_button)
			{
				return;
			}
			else
			{
				cancel_operation();
			}
		}
	}

	if (!DirSymLink && m_DeleteType != delete_type::recycle)
	{
		ScanTree ScTree(true, true, FALSE);

		const auto strSelFullName = IsAbsolutePath(strSelName)?
			strSelName :
			path::join(SrcPanel->GetCurDir(), strSelName);

		ScTree.SetFindPath(strSelFullName, L"*"sv);
		const time_check TreeTimeCheck(time_check::mode::immediate);

		os::fs::find_data FindData;
		string strFullName;
		while (ScTree.GetNextName(FindData,strFullName))
		{
			if (TreeTimeCheck)
				ShellDeleteMsg(strFullName, m_DeleteType == delete_type::erase? DEL_WIPE : DEL_DEL, { ProcessedItems, Total.Items }, 0);

			if (FindData.Attributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (os::fs::is_directory_symbolic_link(FindData))
				{
					if (FindData.Attributes & FILE_ATTRIBUTE_READONLY && !os::fs::set_file_attributes(strFullName, FILE_ATTRIBUTE_NORMAL)) //BUGBUG
					{
						LOGWARNING(L"set_file_attributes({}): {}"sv, strFullName, last_error());
					}

					bool Dummy = false;
					if (!ERemoveDirectory(strFullName, m_DeleteType, Dummy))
					{
						ScTree.SkipDir();
						continue;
					}

					TreeList::DelTreeName(strFullName);

					if (m_UpdateDiz)
						SrcPanel->DeleteDiz(strFullName,strSelShortName);

					continue;
				}

				if (!m_DeleteFolders && !ScTree.IsDirSearchDone() && os::fs::is_not_empty_directory(strFullName))
				{
					const auto MsgCode = Message(MSG_WARNING,
						msg(m_DeleteType == delete_type::erase? lng::MWipeFolderTitle : lng::MDeleteFolderTitle),
						{
							msg(m_DeleteType == delete_type::erase? lng::MWipeFolderConfirm : lng::MDeleteFolderConfirm),
							strFullName
						},
						{ m_DeleteType == delete_type::erase? lng::MDeleteFileWipe : lng::MDeleteFileDelete, lng::MDeleteFileAll, lng::MDeleteFileSkip, lng::MDeleteFileCancel },
						{}, m_DeleteType == delete_type::erase? &WipeFolderId : &DeleteFolderId); // ??? other UUID ???

					if (MsgCode == Message::first_button)
					{
						// Nop
					}
					else if (MsgCode == Message::second_button)
					{
						m_DeleteFolders = true;
					}
					else if (MsgCode == Message::third_button)
					{
						ScTree.SkipDir();
						continue;
					}
					else
					{
						cancel_operation();
					}
				}

				if (ScTree.IsDirSearchDone())
				{
					if (FindData.Attributes & FILE_ATTRIBUTE_READONLY && !os::fs::set_file_attributes(strFullName, FILE_ATTRIBUTE_NORMAL)) //BUGBUG
					{
						LOGWARNING(L"set_file_attributes({}): {}"sv, strFullName, last_error());
					}

					bool Dummy = false;
					if (!ERemoveDirectory(strFullName, m_DeleteType, Dummy))
					{
						//ScTree.SkipDir();
						continue;
					}

					TreeList::DelTreeName(strFullName);
				}
			}
			else
			{
				if (ConfirmDeleteReadOnlyFile(strFullName,FindData.Attributes))
				{
					// BUGBUG check result
					ShellRemoveFile(strFullName, { ProcessedItems, Total.Items });
				}
			}
		}
	}

	if (SelFindData.Attributes & FILE_ATTRIBUTE_READONLY && !os::fs::set_file_attributes(strSelName, FILE_ATTRIBUTE_NORMAL)) //BUGBUG
	{
		LOGWARNING(L"set_file_attributes({}): {}"sv, strSelName, last_error());
	}

	bool RetryRecycleAsRemove = false;
	const auto Removed = ERemoveDirectory(
		strSelName,
		m_DeleteType == delete_type::recycle && DirSymLink && !IsWindowsVistaOrGreater()?
			delete_type::remove :
			m_DeleteType,
		RetryRecycleAsRemove
	);

	if (Removed)
	{
		TreeList::DelTreeName(strSelName);

		if (m_UpdateDiz)
			SrcPanel->DeleteDiz(strSelName,strSelShortName);
	}
	else if (RetryRecycleAsRemove)
	{
		--ProcessedItems;
		m_DeleteType = delete_type::remove;
		process_item(SrcPanel, SelFindData, Total, TimeCheck, true);
		m_DeleteType = delete_type::recycle;
	}

}

ShellDelete::ShellDelete(panel_ptr SrcPanel, delete_type const Type):
	m_DeleteFolders(!Global->Opt->Confirm.DeleteFolder),
	m_UpdateDiz(Global->Opt->Diz.UpdateMode == DIZ_UPDATE_ALWAYS || (SrcPanel->IsDizDisplayed() && Global->Opt->Diz.UpdateMode == DIZ_UPDATE_IF_DISPLAYED)),
	m_DeleteType(Type)
{
	if (m_UpdateDiz)
		SrcPanel->ReadDiz();

	const auto strDizName = SrcPanel->GetDizName();
	const auto CheckDiz = [&] { return !strDizName.empty() && os::fs::exists(strDizName); };
	const auto DizPresent = CheckDiz();

	SCOPED_ACTION(TPreRedrawFuncGuard)(std::make_unique<DelPreRedrawItem>());

	const auto SelCount = SrcPanel->GetSelCount();
	if (!SelCount)
		return;

	os::fs::find_data SingleSelData;
	if (!SrcPanel->get_first_selected(SingleSelData))
		return;

	if (m_DeleteType == delete_type::recycle && os::fs::drive::get_type(GetPathRoot(ConvertNameToFull(SingleSelData.FileName))) != DRIVE_FIXED)
		m_DeleteType = delete_type::remove;

	show_confirmation(SrcPanel, m_DeleteType, SelCount, SingleSelData);

	const auto NeedSetUpADir = CheckUpdateAnotherPanel(SrcPanel, SingleSelData.FileName);

	SCOPE_EXIT
	{
		if (m_UpdateDiz && DizPresent == CheckDiz())
			SrcPanel->FlushDiz();

		ShellUpdatePanels(SrcPanel, NeedSetUpADir);
	};

	if (SrcPanel->GetType() == panel_type::TREE_PANEL)
		FarChDir(L"\\"sv);

	ConsoleTitle::SetFarTitle(msg(lng::MDeletingTitle));
	SCOPED_ACTION(taskbar::indeterminate);
	SCOPED_ACTION(wakeful);
	SetCursorType(false, 0);

	const auto Total = calculate_total(SrcPanel);

	const time_check TimeCheck(time_check::mode::immediate);

	for (const auto& i: SrcPanel->enum_selected())
	{
		process_item(SrcPanel, i, Total, TimeCheck);
	}
}

bool ShellDelete::ConfirmDeleteReadOnlyFile(string_view const Name, os::fs::attributes Attr)
{
	if (!(Attr & FILE_ATTRIBUTE_READONLY))
		return true;

	if (!Global->Opt->Confirm.RO)
		ReadOnlyDeleteMode = Message::first_button;

	int MsgCode;

	if (ReadOnlyDeleteMode != -1)
	{
		MsgCode = ReadOnlyDeleteMode;
	}
	else
	{
		static constexpr std::tuple
			EraseData{ lng::MAskWipeRO, lng::MDeleteFileWipe, &DeleteAskWipeROId },
			DeleteData{ lng::MAskDeleteRO, lng::MDeleteFileDelete, &DeleteAskDeleteROId };

		const auto& [AskId, ButtonId, UidPtr] = m_DeleteType == delete_type::erase? EraseData : DeleteData;

		MsgCode = Message(MSG_WARNING,
			msg(lng::MWarning),
			{
				msg(lng::MDeleteRO),
				string(Name),
				msg(AskId)
			},
			{ ButtonId, lng::MDeleteFileAll, lng::MDeleteFileSkip, lng::MDeleteFileSkipAll, lng::MDeleteFileCancel },
			{}, UidPtr);
	}

	switch (MsgCode)
	{
	case Message::second_button:
		ReadOnlyDeleteMode = Message::first_button;
		[[fallthrough]];
	case Message::first_button:
		if (!os::fs::set_file_attributes(Name, FILE_ATTRIBUTE_NORMAL)) //BUGBUG
		{
			LOGWARNING(L"set_file_attributes({}): {}"sv, Name, last_error());
		}

		return true;

	case Message::fourth_button:
		ReadOnlyDeleteMode = Message::third_button;
		[[fallthrough]];
	case Message::third_button:
		return false;

	default:
		cancel_operation();
	}
}

static bool confirm_erase_file_with_hardlinks(string_view const File, int& WipeMode)
{
	switch (WipeMode != -1? WipeMode : [&]
	{
		const auto Hardlinks = GetNumberOfLinks(File);
		return !Hardlinks || *Hardlinks < 2?
			Message::first_button :
			Message(MSG_WARNING,
				msg(lng::MError),
				{
					string(File),
					msg(lng::MDeleteHardLink1),
					msg(lng::MDeleteHardLink2),
					msg(lng::MDeleteHardLink3)
				},
				{ lng::MDeleteFileWipe, lng::MDeleteFileAll, lng::MDeleteFileSkip, lng::MDeleteFileSkipAll, lng::MDeleteCancel },
				{}, &WipeHardLinkId);
	}())
	{
	case Message::second_button:
		WipeMode = Message::first_button;
		[[fallthrough]];
	case Message::first_button:
		return true;

	case Message::fourth_button:
		WipeMode = Message::third_button;
		[[fallthrough]];
	case Message::third_button:
		return false;

	default:
		cancel_operation();
	}
}

static bool erase_file_with_retry(string_view const Name, int& WipeMode, ShellDelete::progress Files, bool& SkipErrors)
{
	return
		confirm_erase_file_with_hardlinks(Name, WipeMode) &&
		retryable_ui_operation([&]{ return EraseFile(Name, Files); }, Name, lng::MCannotDeleteFile, SkipErrors);
}

static bool delete_file_with_retry(string_view const Name, bool& SkipErrors)
{
	return retryable_ui_operation([&]{ return os::fs::delete_file(Name); }, Name, lng::MCannotDeleteFile, SkipErrors);
}

bool ShellDelete::ShellRemoveFile(string_view const Name, progress Files)
{
	ProcessedItems++;
	const auto strFullName = ConvertNameToFull(Name);

	switch (m_DeleteType)
	{
	case delete_type::erase:
		return erase_file_with_retry(strFullName, SkipWipeMode, Files, m_SkipFileErrors);

	case delete_type::remove:
		return delete_file_with_retry(strFullName, m_SkipFileErrors);

	case delete_type::recycle:
		{
			auto RetryRecycleAsRemove = false, Skip = false;

			const auto Result = retryable_ui_operation([&]{ return RemoveToRecycleBin(strFullName, false, RetryRecycleAsRemove, Skip) || RetryRecycleAsRemove || Skip; }, strFullName, lng::MCannotRecycleFile, m_SkipFileErrors);
			if (Result && !RetryRecycleAsRemove && !Skip)
				return true;

			if (RetryRecycleAsRemove)
				return delete_file_with_retry(strFullName, m_SkipFileErrors);

			return false;
		}

	default:
		UNREACHABLE;
	}
}

bool ShellDelete::ERemoveDirectory(string_view const Name, delete_type const Type, bool& RetryRecycleAsRemove)
{
	ProcessedItems++;

	switch (Type)
	{
	case delete_type::remove:
	case delete_type::erase:
		return retryable_ui_operation([&]{ return (Type == delete_type::erase? EraseDirectory : os::fs::remove_directory)(Name); }, Name, lng::MCannotDeleteFolder, m_SkipFolderErrors);

	case delete_type::recycle:
		{
			auto Skip = false;
			return
				retryable_ui_operation([&]{ return RemoveToRecycleBin(Name, true, RetryRecycleAsRemove, Skip) || RetryRecycleAsRemove || Skip; }, Name, lng::MCannotRecycleFolder, m_SkipFolderErrors) &&
				!Skip &&
				!RetryRecycleAsRemove;
		}
	default:
		UNREACHABLE;
	}
}

static void break_links_for_old_os(string_view const Name)
{
	// При удалении в корзину папки с симлинками получим траблу, если предварительно линки не убрать.
	if (IsWindowsVistaOrGreater() || !os::fs::is_directory(Name))
		return;

	string strFullName2;
	os::fs::find_data FindData;
	ScanTree ScTree(true, true, FALSE);
	ScTree.SetFindPath(Name, L"*"sv);

	bool MessageShown = false;
	bool SkipErrors = false;

	while (ScTree.GetNextName(FindData, strFullName2))
	{
		if (!os::fs::is_directory_symbolic_link(FindData))
			continue;

		if (!MessageShown)
		{
			MessageShown = true;
			if (Message(MSG_WARNING,
				msg(lng::MWarning),
				{
					msg(lng::MRecycleFolderConfirmDeleteLink1),
					msg(lng::MRecycleFolderConfirmDeleteLink2),
					msg(lng::MRecycleFolderConfirmDeleteLink3),
					msg(lng::MRecycleFolderConfirmDeleteLink4)
				},
				{ lng::MYes, lng::MCancel },
				{}, &RecycleFolderConfirmDeleteLinkId
			) != Message::first_button)
			{
				cancel_operation();
			}
		}

		EDeleteReparsePoint(strFullName2, FindData.Attributes, SkipErrors);
	}
}

bool ShellDelete::RemoveToRecycleBin(string_view const Name, bool dir, bool& RetryRecycleAsRemove, bool& Skip)
{
	RetryRecycleAsRemove = false;
	const auto strFullName = ConvertNameToFull(Name);

	break_links_for_old_os(strFullName);

	if (os::fs::move_to_recycle_bin(strFullName))
		return true;

	if (dir? m_SkipFolderErrors : m_SkipFileErrors)
		return false;

	const auto ErrorState = last_error();

	const int MsgCode = Message(MSG_WARNING, ErrorState,
		msg(lng::MError),
		{
			msg(dir? lng::MCannotRecycleFolder : lng::MCannotRecycleFile),
			QuoteOuterSpace(strFullName),
			msg(lng::MTryToDeletePermanently)
		},
		{ lng::MDeleteFileDelete, lng::MDeleteSkip, lng::MDeleteSkipAll, lng::MDeleteCancel },
		{}, dir? &CannotRecycleFolderId : &CannotRecycleFileId);

	switch (MsgCode)
	{
	case Message::first_button:     // {Delete}
		RetryRecycleAsRemove = true;
		return false;

	case Message::third_button:     // [Skip All]
		(dir? m_SkipFolderErrors : m_SkipFileErrors) = true;
		[[fallthrough]];
	case Message::second_button:    // [Skip]
		Skip = true;
		return false;

	default:
		cancel_operation();
	}
}

void DeleteDirTree(string_view const Dir)
{
	if (Dir.empty() ||
	        (Dir.size() == 1 && path::is_separator(Dir[0])) ||
	        (Dir.size() == 3 && Dir[1]==L':' && path::is_separator(Dir[2])))
		return;

	string strFullName;
	os::fs::find_data FindData;
	ScanTree ScTree(true, true, FALSE);
	ScTree.SetFindPath(Dir, L"*"sv);

	while (ScTree.GetNextName(FindData, strFullName))
	{
		if (!os::fs::set_file_attributes(strFullName, FILE_ATTRIBUTE_NORMAL)) //BUGBUG
		{
			LOGWARNING(L"set_file_attributes({}): {}"sv, strFullName, last_error());
		}

		if (FindData.Attributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			// BUGBUG check result
			if (ScTree.IsDirSearchDone() && !os::fs::remove_directory(strFullName))
			{
				LOGWARNING(L"remove_directory({}): {}"sv, strFullName, last_error());
			}
		}
		else
		{
			// BUGBUG check result
			if (!os::fs::delete_file(strFullName))
			{
				LOGWARNING(L"delete_file({}): {}"sv, strFullName, last_error());
			}
		}
	}

	// BUGBUG check result
	if (!os::fs::set_file_attributes(Dir, FILE_ATTRIBUTE_NORMAL))
	{
		LOGWARNING(L"set_file_attributes({}): {}"sv, Dir, last_error());
	}

	// BUGBUG check result
	if (!os::fs::remove_directory(Dir))
	{
		LOGWARNING(L"remove_directory({}): {}"sv, Dir, last_error());
	}

}

bool DeleteFileWithFolder(string_view const FileName)
{
	auto strFileOrFolderName = unquote(FileName);
	if (!os::fs::set_file_attributes(strFileOrFolderName, FILE_ATTRIBUTE_NORMAL)) //BUGBUG
	{
		LOGWARNING(L"set_file_attributes({}): {}"sv, strFileOrFolderName, last_error());
	}

	if (!os::fs::delete_file(strFileOrFolderName))
		return false;

	return CutToParent(strFileOrFolderName) && os::fs::remove_directory(strFileOrFolderName);
}

delayed_deleter::delayed_deleter(bool const WithFolder):
	m_WithFolder(WithFolder)
{
}

delayed_deleter::~delayed_deleter()
{
	for (const auto& i: m_Files)
	{
		if (os::fs::delete_file(i) && m_WithFolder)
		{
			auto Folder = i;
			if (CutToParent(Folder))
			{
				if (!os::fs::remove_directory(Folder))
					LOGWARNING(L"remove_directory({}): {}"sv, i, last_error());
			}
		}
		else
		{
			LOGWARNING(L"delete_file({}): {}"sv, i, last_error());
		}
	}
}

void delayed_deleter::add(string File)
{
	m_Files.emplace_back(std::move(File));
}

bool delayed_deleter::any() const
{
	return !m_Files.empty();
}

void Delete(const panel_ptr& SrcPanel, delete_type const Type)
{
	try
	{
		ShellDelete(SrcPanel, Type);
	}
	catch (const operation_cancelled&)
	{
		// Nop
	}
}
