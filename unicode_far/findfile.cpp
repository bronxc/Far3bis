﻿/*
findfile.cpp

Поиск (Alt-F7)
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

#include "headers.hpp"
#pragma hdrstop

#include "findfile.hpp"
#include "flink.hpp"
#include "keys.hpp"
#include "ctrlobj.hpp"
#include "dialog.hpp"
#include "filepanels.hpp"
#include "panel.hpp"
#include "fileview.hpp"
#include "fileedit.hpp"
#include "filelist.hpp"
#include "cmdline.hpp"
#include "chgprior.hpp"
#include "namelist.hpp"
#include "scantree.hpp"
#include "manager.hpp"
#include "filemasks.hpp"
#include "filefilter.hpp"
#include "syslog.hpp"
#include "encoding.hpp"
#include "cddrv.hpp"
#include "TaskBar.hpp"
#include "interf.hpp"
#include "colormix.hpp"
#include "message.hpp"
#include "delete.hpp"
#include "datetime.hpp"
#include "pathmix.hpp"
#include "strmix.hpp"
#include "mix.hpp"
#include "constitle.hpp"
#include "DlgGuid.hpp"
#include "synchro.hpp"
#include "console.hpp"
#include "wakeful.hpp"
#include "panelmix.hpp"
#include "keyboard.hpp"
#include "plugins.hpp"
#include "lang.hpp"
#include "filestr.hpp"
#include "exitcode.hpp"
#include "panelctype.hpp"
#include "filetype.hpp"
#include "diskmenu.hpp"
#include "local.hpp"
#include "vmenu.hpp"
#include "farexcpt.hpp"
#include "drivemix.hpp"

// Список найденных файлов. Индекс из списка хранится в меню.
struct FindListItem
{
	os::FAR_FIND_DATA FindData;
	FindFiles::ArcListItem* Arc;
	DWORD Used;
	void* Data;
	FARPANELITEMFREECALLBACK FreeData;
};

// TODO BUGBUG DELETE THIS
class InterThreadData
{
private:
	mutable os::critical_section DataCS;
	FindFiles::ArcListItem* FindFileArcItem;
	int Percent;

	std::list<FindListItem> FindList;
	std::list<FindFiles::ArcListItem> ArcList;
	string strFindMessage;

public:
	InterThreadData() {Init();}
	~InterThreadData() { ClearAllLists(); }

	void Init()
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		FindFileArcItem = nullptr;
		Percent=0;
		FindList.clear();
		ArcList.clear();
		strFindMessage.clear();
	}


	FindFiles::ArcListItem* GetFindFileArcItem() const
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		return FindFileArcItem;
	}

	void SetFindFileArcItem(FindFiles::ArcListItem* Value)
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		FindFileArcItem = Value;
	}

	int GetPercent() const { return Percent; }

	void SetPercent(int Value)
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		Percent = Value;
	}

	size_t GetFindListCount() const
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		return FindList.size();
	}

	void GetFindMessage(string& To) const
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		To=strFindMessage;
	}

	void SetFindMessage(const string& From)
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		strFindMessage=From;
	}

	void ClearAllLists()
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		FindFileArcItem = nullptr;

		if (!FindList.empty())
		{
			std::for_each(CONST_RANGE(FindList, i)
			{
				if (i.FreeData)
				{
					FarPanelItemFreeInfo info={sizeof(FarPanelItemFreeInfo),nullptr};
					if(i.Arc)
					{
						info.hPlugin=i.Arc->hPlugin;
					}
					i.FreeData(i.Data,&info);
				}
			});
			FindList.clear();
		}

		ArcList.clear();
	}

	FindFiles::ArcListItem& AddArcListItem(const string& ArcName, plugin_panel* hPlugin, unsigned long long dwFlags, const string& RootPath)
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);

		FindFiles::ArcListItem NewItem;
		NewItem.strArcName = ArcName;
		NewItem.hPlugin = hPlugin;
		NewItem.Flags = dwFlags;
		NewItem.strRootPath = RootPath;
		AddEndSlash(NewItem.strRootPath);
		ArcList.emplace_back(NewItem);
		return ArcList.back();
	}

	FindListItem& AddFindListItem(const os::FAR_FIND_DATA& FindData, void* Data, FARPANELITEMFREECALLBACK FreeData)
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);

		FindListItem NewItem;
		NewItem.FindData = FindData;
		NewItem.Arc = nullptr;
		NewItem.Data = Data;
		NewItem.FreeData = FreeData;
		FindList.emplace_back(NewItem);
		return FindList.back();
	}

	template <typename Visitor>
	void ForEachFindItem(const Visitor& visitor) const
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		for (const auto& i: FindList)
			visitor(i);
	}

	template <typename Visitor>
	void ForEachFindItem(const Visitor& visitor)
	{
		SCOPED_ACTION(os::critical_section_lock)(DataCS);
		for (auto& i: FindList)
			visitor(i);
	}
};


enum
{
	FIND_EXIT_NONE,
	FIND_EXIT_SEARCHAGAIN,
	FIND_EXIT_GOTO,
	FIND_EXIT_PANEL
};

enum ADVANCEDDLG
{
	AD_DOUBLEBOX,
	AD_TEXT_SEARCHFIRST,
	AD_EDIT_SEARCHFIRST,
	AD_SEPARATOR1,
	AD_TEXT_COLUMNSFORMAT,
	AD_EDIT_COLUMNSFORMAT,
	AD_TEXT_COLUMNSWIDTH,
	AD_EDIT_COLUMNSWIDTH,
	AD_SEPARATOR2,
	AD_BUTTON_OK,
	AD_BUTTON_CANCEL,
};

enum FINDASKDLG
{
	FAD_DOUBLEBOX,
	FAD_TEXT_MASK,
	FAD_EDIT_MASK,
	FAD_SEPARATOR0,
	FAD_TEXT_TEXTHEX,
	FAD_EDIT_TEXT,
	FAD_EDIT_HEX,
	FAD_TEXT_CP,
	FAD_COMBOBOX_CP,
	FAD_SEPARATOR1,
	FAD_CHECKBOX_CASE,
	FAD_CHECKBOX_WHOLEWORDS,
	FAD_CHECKBOX_HEX,
	FAD_CHECKBOX_NOTCONTAINING,
	FAD_CHECKBOX_ARC,
	FAD_CHECKBOX_DIRS,
	FAD_CHECKBOX_LINKS,
	FAD_CHECKBOX_STREAMS,
	FAD_SEPARATOR_2,
	FAD_SEPARATOR_3,
	FAD_TEXT_WHERE,
	FAD_COMBOBOX_WHERE,
	FAD_CHECKBOX_FILTER,
	FAD_SEPARATOR_4,
	FAD_BUTTON_FIND,
	FAD_BUTTON_DRIVE,
	FAD_BUTTON_FILTER,
	FAD_BUTTON_ADVANCED,
	FAD_BUTTON_CANCEL,
};

enum FINDASKDLGCOMBO
{
	FADC_ALLDISKS,
	FADC_ALLBUTNET,
	FADC_PATH,
	FADC_ROOT,
	FADC_FROMCURRENT,
	FADC_INCURRENT,
	FADC_SELECTED,
};

enum FINDDLG
{
	FD_DOUBLEBOX,
	FD_LISTBOX,
	FD_SEPARATOR1,
	FD_TEXT_STATUS,
	FD_TEXT_STATUS_PERCENTS,
	FD_SEPARATOR2,
	FD_BUTTON_NEW,
	FD_BUTTON_GOTO,
	FD_BUTTON_VIEW,
	FD_BUTTON_PANEL,
	FD_BUTTON_STOP,
};

struct background_searcher::CodePageInfo
{
	CodePageInfo(uintptr_t CodePage):
		CodePage(CodePage),
		MaxCharSize(0),
		LastSymbol(0),
		WordFound(false)
	{
	}

	uintptr_t CodePage;
	UINT MaxCharSize;
	wchar_t LastSymbol;
	bool WordFound;

	void initialize()
	{
		if (IsUnicodeCodePage(CodePage))
			MaxCharSize = 2;
		else
		{
			CPINFO cpi;

			if (!GetCPInfo(CodePage, &cpi))
				cpi.MaxCharSize = 0; //Считаем, что ошибка и потом такие таблицы в поиске пропускаем

			MaxCharSize = cpi.MaxCharSize;
		}

		LastSymbol = 0;
		WordFound = false;
	}
};

void background_searcher::InitInFileSearch()
{
	if (!InFileSearchInited && !strFindStr.empty())
	{
		size_t findStringCount = strFindStr.size();
		// Инициализируем буферы чтения из файла
		const size_t readBufferSize = 32768;

		readBufferA.resize(readBufferSize);
		readBuffer.resize(readBufferSize);

		if (!SearchHex)
		{
			// Формируем строку поиска
			if (!CmpCase)
			{
				findStringBuffer = Upper(strFindStr) + Lower(strFindStr);
				findString = findStringBuffer.data();
			}
			else
				findString = strFindStr.data();

			// Инициализируем данные для алгоритма поиска
			skipCharsTable.assign(std::numeric_limits<wchar_t>::max() + 1, findStringCount);

			for (size_t index = 0; index < findStringCount-1; index++)
				skipCharsTable[findString[index]] = findStringCount-1-index;

			if (!CmpCase)
				for (size_t index = 0; index < findStringCount-1; index++)
					skipCharsTable[findString[index+findStringCount]] = findStringCount-1-index;

			// Формируем список кодовых страниц
			if (CodePage == CP_SET)
			{
				// Проверяем наличие выбранных страниц символов
				const auto CpEnum = Codepages().GetFavoritesEnumerator();
				const auto hasSelected = std::any_of(CONST_RANGE(CpEnum, i) { return i.second & CPST_FIND; });

				if (hasSelected)
				{
					m_CodePages.clear();
				}
				else
				{
					// Добавляем стандартные таблицы символов
					const uintptr_t Predefined[] = { GetOEMCP(), GetACP(), CP_UTF8, CP_UNICODE, CP_REVERSEBOM };
					m_CodePages.insert(m_CodePages.end(), ALL_CONST_RANGE(Predefined));
				}

				// Добавляем избранные таблицы символов
				std::for_each(CONST_RANGE(CpEnum, i)
				{
					if (i.second & (hasSelected?CPST_FIND:CPST_FAVORITE))
					{
						uintptr_t codePage = std::stoi(i.first);

						// Проверяем дубли
						if (hasSelected || !std::any_of(CONST_RANGE(m_CodePages, cp) { return cp.CodePage == CodePage; }))
							m_CodePages.emplace_back(codePage);
					}
				});
			}
			else
			{
				m_CodePages.emplace_back(CodePage);
				m_Autodetection = CodePage == CP_DEFAULT;
			}

			std::for_each(RANGE(m_CodePages, i)
			{
				i.initialize();
			});
		}
		else
		{
			// Формируем hex-строку для поиска
			if (SearchHex)
			{
				hexFindString = HexStringToBlob(strFindStr.data(), 0);
			}

			// Инициализируем данные для аглоритма поиска
			skipCharsTable.assign(std::numeric_limits<unsigned char>::max() + 1, hexFindString.size());

			for (size_t index = 0; index < hexFindString.size() - 1; index++)
				skipCharsTable[hexFindString[index]] = hexFindString.size() - 1 - index;
		}

		InFileSearchInited=true;
	}
}

void background_searcher::ReleaseInFileSearch()
{
	if (InFileSearchInited && !strFindStr.empty())
	{
		clear_and_shrink(readBufferA);
		clear_and_shrink(readBuffer);
		clear_and_shrink(skipCharsTable);
		hexFindString.clear();
		m_CodePages.clear();
		InFileSearchInited=false;
	}
}

string& FindFiles::PrepareDriveNameStr(string &strSearchFromRoot) const
{
	auto strCurDir = GetPathRoot(Global->CtrlObject->CmdLine()->GetCurDir());
	DeleteEndSlash(strCurDir);

	if (
	    strCurDir.empty()||
		(Global->CtrlObject->Cp()->ActivePanel()->GetMode() == panel_mode::PLUGIN_PANEL && Global->CtrlObject->Cp()->ActivePanel()->IsVisible())
	)
	{
		strSearchFromRoot = msg(lng::MSearchFromRootFolder);
	}
	else
	{
		strSearchFromRoot = concat(msg(lng::MSearchFromRootOfDrive), L' ', strCurDir);
	}

	return strSearchFromRoot;
}

// Проверяем символ на принадлежность разделителям слов
bool FindFiles::IsWordDiv(const wchar_t symbol)
{
	// Так же разделителем является конец строки и пробельные символы
	return !symbol||IsSpace(symbol)||IsEol(symbol)||::IsWordDiv(Global->Opt->strWordDiv,symbol);
}

#if defined(MANTIS_0002207)
static intptr_t GetUserDataFromPluginItem(const wchar_t *Name, const PluginPanelItem * const* PanelData,size_t ItemCount)
{
	intptr_t UserData=0;

	if (Name && *Name)
	{
		for (size_t Index=0; Index < ItemCount; ++Index)
		{
			if (!StrCmp(PanelData[Index]->FileName,Name))
			{
				UserData=(intptr_t)PanelData[Index]->UserData.Data;
				break;
			}
		}
	}

	return UserData;
}
#endif

void FindFiles::SetPluginDirectory(const string& DirName, plugin_panel* hPlugin, bool UpdatePanel, UserDataItem *UserData)
{
	if (!DirName.empty())
	{
		string strName(DirName);
		//const wchar_t* DirPtr = ;
		const wchar_t* NamePtr = PointToName(strName.data());

		if (NamePtr != strName.data())
		{
			string Dir = strName.substr(0, NamePtr - strName.data());

			// force plugin to update its file list (that can be empty at this time)
			// if not done SetDirectory may fail
			{
				size_t FileCount=0;
				PluginPanelItem *PanelData=nullptr;

				SCOPED_ACTION(os::critical_section_lock)(PluginCS);
				if (Global->CtrlObject->Plugins->GetFindData(hPlugin,&PanelData,&FileCount,OPM_SILENT))
					Global->CtrlObject->Plugins->FreeFindData(hPlugin,PanelData,FileCount,true);
			}

			DeleteEndSlash(Dir);

			SCOPED_ACTION(os::critical_section_lock)(PluginCS);
			Global->CtrlObject->Plugins->SetDirectory(hPlugin, Dir.empty()? L"\\" : Dir.data(), OPM_SILENT, Dir.empty()? nullptr : UserData);
		}

		// Отрисуем панель при необходимости.
		if (UpdatePanel)
		{
			Global->CtrlObject->Cp()->ActivePanel()->Update(UPDATE_KEEP_SELECTION);
			Global->CtrlObject->Cp()->ActivePanel()->GoToFile(NamePtr);
			Global->CtrlObject->Cp()->ActivePanel()->Show();
		}
	}
}

intptr_t FindFiles::AdvancedDlgProc(Dialog* Dlg, intptr_t Msg, intptr_t Param1, void* Param2)
{
	switch (Msg)
	{
		case DN_CLOSE:

			if (Param1==AD_BUTTON_OK)
			{
				const auto Data = reinterpret_cast<const wchar_t*>(Dlg->SendMessage(DM_GETCONSTTEXTPTR, AD_EDIT_SEARCHFIRST, nullptr));

				if (Data && *Data && !CheckFileSizeStringFormat(Data))
				{
					Message(MSG_WARNING,1,msg(lng::MFindFileAdvancedTitle),msg(lng::MBadFileSizeFormat),msg(lng::MOk));
					return FALSE;
				}
			}

			break;
		default:
			break;
	}

	return Dlg->DefProc(Msg,Param1,Param2);
}

void FindFiles::AdvancedDialog()
{
	FarDialogItem AdvancedDlgData[]=
	{
		{DI_DOUBLEBOX,3,1,52,11,0,nullptr,nullptr,0,msg(lng::MFindFileAdvancedTitle)},
		{DI_TEXT,5,2,0,2,0,nullptr,nullptr,0,msg(lng::MFindFileSearchFirst)},
		{DI_EDIT,5,3,50,3,0,nullptr,nullptr,0,Global->Opt->FindOpt.strSearchInFirstSize.data()},
		{DI_TEXT,-1,4,0,4,0,nullptr,nullptr,DIF_SEPARATOR,L""},
		{DI_TEXT,5,5, 0, 5,0,nullptr,nullptr,0,msg(lng::MFindAlternateModeTypes)},
		{DI_EDIT,5,6,50, 6,0,nullptr,nullptr,0,Global->Opt->FindOpt.strSearchOutFormat.data()},
		{DI_TEXT,5,7, 0, 7,0,nullptr,nullptr,0,msg(lng::MFindAlternateModeWidths)},
		{DI_EDIT,5,8,50, 8,0,nullptr,nullptr,0,Global->Opt->FindOpt.strSearchOutFormatWidth.data()},
		{DI_TEXT,-1,9,0,9,0,nullptr,nullptr,DIF_SEPARATOR,L""},
		{DI_BUTTON,0,10,0,10,0,nullptr,nullptr,DIF_DEFAULTBUTTON|DIF_CENTERGROUP,msg(lng::MOk)},
		{DI_BUTTON,0,10,0,10,0,nullptr,nullptr,DIF_CENTERGROUP,msg(lng::MCancel)},
	};
	auto AdvancedDlg = MakeDialogItemsEx(AdvancedDlgData);
	const auto Dlg = Dialog::create(AdvancedDlg, &FindFiles::AdvancedDlgProc);
	Dlg->SetHelp(L"FindFileAdvanced");
	Dlg->SetPosition(-1,-1,52+4,13);
	Dlg->Process();
	int ExitCode=Dlg->GetExitCode();

	if (ExitCode==AD_BUTTON_OK)
	{
		Global->Opt->FindOpt.strSearchInFirstSize = AdvancedDlg[AD_EDIT_SEARCHFIRST].strData;
		SearchInFirst=ConvertFileSizeString(Global->Opt->FindOpt.strSearchInFirstSize);

		Global->Opt->SetSearchColumns(AdvancedDlg[AD_EDIT_COLUMNSFORMAT].strData, AdvancedDlg[AD_EDIT_COLUMNSWIDTH].strData);
	}
}

intptr_t FindFiles::MainDlgProc(Dialog* Dlg, intptr_t Msg, intptr_t Param1, void* Param2)
{
	const auto& SetAllCpTitle = [&]()
	{
		const int TitlePosition = 1;
		const auto CpEnum = Codepages().GetFavoritesEnumerator();
		const auto Title = msg(std::any_of(CONST_RANGE(CpEnum, i) { return i.second & CPST_FIND; })? lng::MFindFileSelectedCodePages : lng::MFindFileAllCodePages);
		Dlg->GetAllItem()[FAD_COMBOBOX_CP].ListPtr->at(TitlePosition).strName = Title;
		FarListPos Position = { sizeof(FarListPos) };
		Dlg->SendMessage(DM_LISTGETCURPOS, FAD_COMBOBOX_CP, &Position);
		if (Position.SelectPos == TitlePosition)
			Dlg->SendMessage(DM_SETTEXTPTR, FAD_COMBOBOX_CP, const_cast<wchar_t*>(Title));
	};

	switch (Msg)
	{
		case DN_INITDIALOG:
		{
			bool Hex = (Dlg->SendMessage(DM_GETCHECK, FAD_CHECKBOX_HEX, nullptr) == BSTATE_CHECKED);
			Dlg->SendMessage(DM_SHOWITEM,FAD_EDIT_TEXT,ToPtr(!Hex));
			Dlg->SendMessage(DM_SHOWITEM,FAD_EDIT_HEX,ToPtr(Hex));
			Dlg->SendMessage(DM_ENABLE,FAD_TEXT_CP,ToPtr(!Hex));
			Dlg->SendMessage(DM_ENABLE,FAD_COMBOBOX_CP,ToPtr(!Hex));
			Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_CASE,ToPtr(!Hex));
			Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_WHOLEWORDS,ToPtr(!Hex));
			Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_DIRS,ToPtr(!Hex));
			Dlg->SendMessage(DM_EDITUNCHANGEDFLAG,FAD_EDIT_TEXT,ToPtr(1));
			Dlg->SendMessage(DM_EDITUNCHANGEDFLAG,FAD_EDIT_HEX,ToPtr(1));
			Dlg->SendMessage(DM_SETTEXTPTR,FAD_TEXT_TEXTHEX,const_cast<wchar_t*>(Hex?msg(lng::MFindFileHex):msg(lng::MFindFileText)));
			Dlg->SendMessage(DM_SETTEXTPTR,FAD_TEXT_CP,const_cast<wchar_t*>(msg(lng::MFindFileCodePage)));
			Dlg->SendMessage(DM_SETCOMBOBOXEVENT,FAD_COMBOBOX_CP,ToPtr(CBET_KEY));
			FarListTitles Titles={sizeof(FarListTitles),0,nullptr,0,msg(lng::MFindFileCodePageBottom)};
			Dlg->SendMessage(DM_LISTSETTITLES,FAD_COMBOBOX_CP,&Titles);
			// Установка запомненных ранее параметров
			CodePage = Global->Opt->FindCodePage;
			favoriteCodePages = Codepages().FillCodePagesList(Dlg, FAD_COMBOBOX_CP, CodePage, true, true, false, true, false);
			SetAllCpTitle();

			// Текущее значение в списке выбора кодовых страниц в общем случае может не совпадать с CodePage,
			// так что получаем CodePage из списка выбора
			FarListPos Position={sizeof(FarListPos)};
			Dlg->SendMessage( DM_LISTGETCURPOS, FAD_COMBOBOX_CP, &Position);
			FarListGetItem Item = { sizeof(FarListGetItem), Position.SelectPos };
			Dlg->SendMessage( DM_LISTGETITEM, FAD_COMBOBOX_CP, &Item);
			CodePage = *Dlg->GetListItemDataPtr<uintptr_t>(FAD_COMBOBOX_CP, Position.SelectPos);
			return TRUE;
		}
		case DN_CLOSE:
		{
			switch (Param1)
			{
				case FAD_BUTTON_FIND:
				{
					string Mask((LPCWSTR)Dlg->SendMessage(DM_GETCONSTTEXTPTR, FAD_EDIT_MASK, nullptr));

					if (Mask.empty())
						Mask=L"*";

					return FileMaskForFindFile->Set(Mask);
				}
				case FAD_BUTTON_DRIVE:
				{
					ChangeDisk(Global->CtrlObject->Cp()->ActivePanel());
					// Ну что ж, раз пошла такая пьянка рефрешить окна
					// будем таким способом.
					Global->WindowManager->ResizeAllWindows();
					string strSearchFromRoot;
					PrepareDriveNameStr(strSearchFromRoot);
					FarListGetItem item={sizeof(FarListGetItem),FADC_ROOT};
					Dlg->SendMessage(DM_LISTGETITEM,FAD_COMBOBOX_WHERE,&item);
					item.Item.Text=strSearchFromRoot.data();
					Dlg->SendMessage(DM_LISTUPDATE,FAD_COMBOBOX_WHERE,&item);
					PluginMode = Global->CtrlObject->Cp()->ActivePanel()->GetMode() == panel_mode::PLUGIN_PANEL;
					Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_DIRS,ToPtr(!PluginMode));
					item.ItemIndex=FADC_ALLDISKS;
					Dlg->SendMessage(DM_LISTGETITEM,FAD_COMBOBOX_WHERE,&item);

					if (PluginMode)
						item.Item.Flags|=LIF_GRAYED;
					else
						item.Item.Flags&=~LIF_GRAYED;

					Dlg->SendMessage(DM_LISTUPDATE,FAD_COMBOBOX_WHERE,&item);
					item.ItemIndex=FADC_ALLBUTNET;
					Dlg->SendMessage(DM_LISTGETITEM,FAD_COMBOBOX_WHERE,&item);

					if (PluginMode)
						item.Item.Flags|=LIF_GRAYED;
					else
						item.Item.Flags&=~LIF_GRAYED;

					Dlg->SendMessage(DM_LISTUPDATE,FAD_COMBOBOX_WHERE,&item);
				}
				break;
				case FAD_BUTTON_FILTER:
					Filter->FilterEdit();
					break;
				case FAD_BUTTON_ADVANCED:
					AdvancedDialog();
					break;
				case -2:
				case -1:
				case FAD_BUTTON_CANCEL:
					return TRUE;
			}

			return FALSE;
		}
		case DN_BTNCLICK:
		{
			switch (Param1)
			{
				case FAD_CHECKBOX_DIRS:
					{
						FindFoldersChanged = true;
					}
					break;

				case FAD_CHECKBOX_HEX:
				{
					Dlg->SendMessage(DM_ENABLEREDRAW, FALSE, nullptr);
					const auto Src = reinterpret_cast<const wchar_t*>(Dlg->SendMessage(DM_GETCONSTTEXTPTR, Param2 ? FAD_EDIT_TEXT : FAD_EDIT_HEX, nullptr));
					const auto strDataStr = ConvertHexString(Src, CodePage, !Param2);
					Dlg->SendMessage(DM_SETTEXTPTR,Param2?FAD_EDIT_HEX:FAD_EDIT_TEXT, UNSAFE_CSTR(strDataStr));
					const auto iParam = reinterpret_cast<intptr_t>(Param2);
					Dlg->SendMessage(DM_SHOWITEM,FAD_EDIT_TEXT,ToPtr(!iParam));
					Dlg->SendMessage(DM_SHOWITEM,FAD_EDIT_HEX,ToPtr(iParam));
					Dlg->SendMessage(DM_ENABLE,FAD_TEXT_CP,ToPtr(!iParam));
					Dlg->SendMessage(DM_ENABLE,FAD_COMBOBOX_CP,ToPtr(!iParam));
					Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_CASE,ToPtr(!iParam));
					Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_WHOLEWORDS,ToPtr(!iParam));
					Dlg->SendMessage(DM_ENABLE,FAD_CHECKBOX_DIRS,ToPtr(!iParam));
					Dlg->SendMessage(DM_SETTEXTPTR,FAD_TEXT_TEXTHEX, const_cast<wchar_t*>(Param2?msg(lng::MFindFileHex):msg(lng::MFindFileText)));

					if (strDataStr.size()>0)
					{
						int UnchangeFlag=(int)Dlg->SendMessage(DM_EDITUNCHANGEDFLAG,FAD_EDIT_TEXT,ToPtr(-1));
						Dlg->SendMessage(DM_EDITUNCHANGEDFLAG,FAD_EDIT_HEX,ToPtr(UnchangeFlag));
					}

					Dlg->SendMessage(DM_ENABLEREDRAW, TRUE, nullptr);
				}
				break;
			}

			break;
		}
		case DN_CONTROLINPUT:
		{
			const auto record = static_cast<const INPUT_RECORD*>(Param2);
			if (record->EventType!=KEY_EVENT) break;
			int key = InputRecordToKey(record);
			switch (Param1)
			{
				case FAD_COMBOBOX_CP:
				{
					switch (key)
					{
						case KEY_INS:
						case KEY_NUMPAD0:
						case KEY_SPACE:
						{
							// Обработка установки/снятия флажков для стандартных и избранных таблиц символов
							// Получаем текущую позицию в выпадающем списке таблиц символов
							FarListPos Position={sizeof(FarListPos)};
							Dlg->SendMessage( DM_LISTGETCURPOS, FAD_COMBOBOX_CP, &Position);
							// Получаем номер выбранной таблицы символов
							FarListGetItem Item = { sizeof(FarListGetItem), Position.SelectPos };
							Dlg->SendMessage( DM_LISTGETITEM, FAD_COMBOBOX_CP, &Item);
							const auto SelectedCodePage = *Dlg->GetListItemDataPtr<uintptr_t>(FAD_COMBOBOX_CP, Position.SelectPos);
							// Разрешаем отмечать только стандартные и избранные таблицы символов
							int FavoritesIndex = 2 + StandardCPCount + 2;

							if (Position.SelectPos > 1)
							{
								// Получаем текущее состояние флага в реестре
								long long SelectType = Codepages().GetFavorite(SelectedCodePage);

								// Отмечаем/разотмечаем таблицу символов
								if (Item.Item.Flags & LIF_CHECKED)
								{
									// Для стандартных таблиц символов просто удаляем значение из реестра, для
									// избранных же оставляем в реестре флаг, что таблица символов избранная
									if (SelectType & CPST_FAVORITE)
										Codepages().SetFavorite(SelectedCodePage, CPST_FAVORITE);
									else
										Codepages().DeleteFavorite(SelectedCodePage);

									Item.Item.Flags &= ~LIF_CHECKED;
								}
								else
								{
									Codepages().SetFavorite(SelectedCodePage, CPST_FIND | (SelectType & CPST_FAVORITE ? CPST_FAVORITE : 0));
									Item.Item.Flags |= LIF_CHECKED;
								}

								SetAllCpTitle();

								// Обновляем текущий элемент в выпадающем списке
								Dlg->SendMessage( DM_LISTUPDATE, FAD_COMBOBOX_CP, &Item);

								FarListPos Pos={sizeof(FarListPos),Position.SelectPos+1,Position.TopPos};
								Dlg->SendMessage( DM_LISTSETCURPOS, FAD_COMBOBOX_CP,&Pos);

								// Обрабатываем случай, когда таблица символов может присутствовать, как в стандартных, так и в избранных,
								// т.е. выбор/снятие флага автоматически происходит у обоих элементов
								bool bStandardCodePage = Position.SelectPos < FavoritesIndex;

								for (int Index = bStandardCodePage ? FavoritesIndex : 0; Index < (bStandardCodePage ? FavoritesIndex + favoriteCodePages : FavoritesIndex); Index++)
								{
									// Получаем элемент таблицы символов
									FarListGetItem CheckItem = { sizeof(FarListGetItem), Index };
									Dlg->SendMessage( DM_LISTGETITEM, FAD_COMBOBOX_CP, &CheckItem);

									// Обрабатываем только таблицы символов
									if (!(CheckItem.Item.Flags&LIF_SEPARATOR))
									{
										if (SelectedCodePage == *Dlg->GetListItemDataPtr<uintptr_t>(FAD_COMBOBOX_CP, Index))
										{
											if (Item.Item.Flags & LIF_CHECKED)
												CheckItem.Item.Flags |= LIF_CHECKED;
											else
												CheckItem.Item.Flags &= ~LIF_CHECKED;

											Dlg->SendMessage( DM_LISTUPDATE, FAD_COMBOBOX_CP, &CheckItem);
											break;
										}
									}
								}
							}
						}
						break;
					}
				}
				break;
			}

			break;
		}
		case DN_EDITCHANGE:
		{
			auto& Item=*reinterpret_cast<FarDialogItem*>(Param2);

			switch (Param1)
			{
				case FAD_EDIT_TEXT:
					// Строка "Содержащих текст"
					if (!FindFoldersChanged)
					{
						const auto Checked = Item.Data && *Item.Data? false : Global->Opt->FindOpt.FindFolders;
						Dlg->SendMessage( DM_SETCHECK, FAD_CHECKBOX_DIRS, ToPtr(Checked? BSTATE_CHECKED : BSTATE_UNCHECKED));
					}
					return TRUE;

				case FAD_COMBOBOX_CP:
					// Получаем выбранную в выпадающем списке таблицу символов
					CodePage = *Dlg->GetListItemDataPtr<uintptr_t>(FAD_COMBOBOX_CP, Dlg->SendMessage(DM_LISTGETCURPOS, FAD_COMBOBOX_CP, nullptr));
					return TRUE;

				case FAD_COMBOBOX_WHERE:
					SearchFromChanged = true;
					return TRUE;
			}
		}
		case DN_HOTKEY:
			if (Param1==FAD_TEXT_TEXTHEX)
			{
				Dlg->SendMessage(DM_SETFOCUS, FAD_EDIT_HEX, nullptr); // only one
				Dlg->SendMessage(DM_SETFOCUS, FAD_EDIT_TEXT, nullptr); // is active
				return FALSE;
			}

		default:
			break;
	}

	return Dlg->DefProc(Msg,Param1,Param2);
}

bool FindFiles::GetPluginFile(ArcListItem* ArcItem, const os::FAR_FIND_DATA& FindData, const string& DestPath, string &strResultName, UserDataItem *UserData)
{
	SCOPED_ACTION(os::critical_section_lock)(PluginCS);

	_ALGO(CleverSysLog clv(L"FindFiles::GetPluginFile()"));
	OpenPanelInfo Info;

	Global->CtrlObject->Plugins->GetOpenPanelInfo(ArcItem->hPlugin,&Info);
	string strSaveDir = NullToEmpty(Info.CurDir);
	AddEndSlash(strSaveDir);
	Global->CtrlObject->Plugins->SetDirectory(ArcItem->hPlugin,L"\\",OPM_SILENT);
	SetPluginDirectory(FindData.strFileName,ArcItem->hPlugin,false,UserData);
	const auto FileNameToFind = PointToName(FindData.strFileName);
	const auto FileNameToFindShort = PointToName(FindData.strAlternateFileName);
	PluginPanelItem *Items;
	size_t ItemsNumber;
	bool nResult=false;

	if (Global->CtrlObject->Plugins->GetFindData(ArcItem->hPlugin, &Items, &ItemsNumber, OPM_SILENT))
	{
		const auto End = Items + ItemsNumber;
		const auto It = std::find_if(Items, End, [&](const auto& Item)
		{
			return !StrCmp(FileNameToFind, NullToEmpty(Item.FileName)) && !StrCmp(FileNameToFindShort, NullToEmpty(Item.AlternateFileName));
		});

		if (It != End)
		{
			nResult = Global->CtrlObject->Plugins->GetFile(ArcItem->hPlugin, &*It, DestPath, strResultName, OPM_SILENT) != 0;
		}

		Global->CtrlObject->Plugins->FreeFindData(ArcItem->hPlugin, Items, ItemsNumber, true);
	}

	Global->CtrlObject->Plugins->SetDirectory(ArcItem->hPlugin,L"\\",OPM_SILENT);
	SetPluginDirectory(strSaveDir,ArcItem->hPlugin);
	return nResult;
}


// Алгоритма Бойера-Мура-Хорспула поиска подстроки
template<typename char_type, typename predicate>
static int FindStringBMH_Impl(const char_type* searchBuffer, size_t searchBufferCount, size_t findStringCount, const std::vector<size_t>& skipCharsTable, predicate Predicate)
{
	auto buffer = searchBuffer;
	const auto lastBufferChar = findStringCount - 1;

	while (searchBufferCount >= findStringCount)
	{
		for (auto index = lastBufferChar; Predicate(buffer, index); --index)
			if (!index)
				return static_cast<int>(buffer - searchBuffer);

		const auto offset = skipCharsTable[buffer[lastBufferChar]];
		searchBufferCount -= offset;
		buffer += offset;
	}

	return -1;
}

int background_searcher::FindStringBMH(const wchar_t* searchBuffer, size_t searchBufferCount) const
{
	const auto findStringCount = strFindStr.size();
	const auto findStringLower = CmpCase? nullptr : findString + findStringCount;

	return FindStringBMH_Impl(searchBuffer, searchBufferCount, findStringCount, skipCharsTable, [&](const wchar_t* Buffer, size_t index)
	{
		return Buffer[index] == findString[index] || (findStringLower && Buffer[index] == findStringLower[index]);
	});
}

int background_searcher::FindStringBMH(const char* searchBuffer, size_t searchBufferCount) const
{
	return FindStringBMH_Impl(searchBuffer, searchBufferCount, hexFindString.size(), skipCharsTable, [&](const char* Buffer, size_t index)
	{
		return Buffer[index] == hexFindString[index];
	});
}


bool background_searcher::LookForString(const string& Name)
{
	// Длина строки поиска
	size_t findStringCount = strFindStr.size();

	// Если строки поиска пустая, то считаем, что мы всегда что-нибудь найдём
	if (!findStringCount)
		return true;

	// Открываем файл
	os::fs::file File(Name, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
	if(!File)
	{
		return false;
	}

	if (m_Autodetection)
	{
		if (!GetFileFormat(File, m_CodePages.front().CodePage))
		{
			// TODO diagnostic message
			m_CodePages.front().CodePage = GetACP();
		}
		m_CodePages.front().initialize();
	}

	// Количество считанных из файла байт
	size_t readBlockSize = 0;
	// Количество прочитанных из файла байт
	unsigned long long alreadyRead = 0;
	// Смещение на которое мы отступили при переходе между блоками
	size_t offset = 0;

	if (SearchHex)
		offset = hexFindString.size() - 1;

	unsigned long long FileSize = 0;
	File.GetSize(FileSize);

	if (SearchInFirst)
	{
		FileSize=std::min(SearchInFirst,FileSize);
	}

	UINT LastPercents=0;

	// Основной цикл чтения из файла
	while (!Stopped() && File.Read(readBufferA.data(), (!SearchInFirst || alreadyRead + readBufferA.size() <= SearchInFirst)? readBufferA.size() : SearchInFirst - alreadyRead, readBlockSize))
	{
		UINT Percents=static_cast<UINT>(FileSize?alreadyRead*100/FileSize:0);

		if (Percents!=LastPercents)
		{
			m_Owner->itd->SetPercent(Percents);
			LastPercents=Percents;
		}

		// Увеличиваем счётчик прочитаннх байт
		alreadyRead += readBlockSize;

		// Для hex и обыкновенного поиска разные ветки
		if (SearchHex)
		{
			// Выходим, если ничего не прочитали или прочитали мало
			if (!readBlockSize || readBlockSize < hexFindString.size())
				return false;

			// Ищем
			if (FindStringBMH(readBufferA.data(), readBlockSize) != -1)
				return true;
		}
		else
		{
			bool ErrorState = false;
			for (auto& i: m_CodePages)
			{
				ErrorState = false;
				// Пропускаем ошибочные кодовые страницы
				if (!i.MaxCharSize)
				{
					ErrorState = true;
					continue;
				}

				// Если начало файла очищаем информацию о поиске по словам
				if (WholeWords && alreadyRead==readBlockSize)
				{
					i.WordFound = false;
					i.LastSymbol = 0;
				}

				// Если ничего не прочитали
				if (!readBlockSize)
				{
					// Если поиск по словам и в конце предыдущего блока было что-то найдено,
					// то считаем, что нашли то, что нужно
					if(WholeWords && i.WordFound)
						return true;
					else
					{
						ErrorState = true;
						continue;
					}
					// Выходим, если прочитали меньше размера строки поиска и нет поиска по словам
				}

				if (readBlockSize < findStringCount && !(WholeWords && i.WordFound))
				{
					ErrorState = true;
					continue;
				}

				// Количество символов в выходном буфере
				size_t bufferCount;

				// Буфер для поиска
				wchar_t *buffer;

				// Перегоняем буфер в UTF-16
				if (IsUnicodeCodePage(i.CodePage))
				{
					// Вычисляем размер буфера в UTF-16
					bufferCount = readBlockSize/sizeof(wchar_t);

					// Выходим, если размер буфера меньше длины строки поиска
					if (bufferCount < findStringCount)
					{
						ErrorState = true;
						continue;
					}

					// Копируем буфер чтения в буфер сравнения
					if (i.CodePage==CP_REVERSEBOM)
					{
						// Для UTF-16 (big endian) преобразуем буфер чтения в буфер сравнения
						swap_bytes(readBufferA.data(), readBuffer.data(), readBlockSize);
						// Устанавливаем буфер сравнения
						buffer = readBuffer.data();
					}
					else
					{
						// Если поиск в UTF-16 (little endian), то используем исходный буфер
						buffer = reinterpret_cast<wchar_t*>(readBufferA.data());
					}
				}
				else
				{
					// Конвертируем буфер чтения из кодировки поиска в UTF-16
					bufferCount = encoding::get_chars(i.CodePage, readBufferA.data(), readBlockSize, readBuffer);

					// Выходим, если нам не удалось сконвертировать строку
					if (!bufferCount)
					{
						ErrorState = true;
						continue;
					}

					// Если у нас поиск по словам и в конце предыдущего блока было вхождение
					if (WholeWords && i.WordFound)
					{
						// Если конец файла, то считаем, что есть разделитель в конце
						if (findStringCount-1>=bufferCount)
							return true;

						// Проверяем первый символ текущего блока с учётом обратного смещения, которое делается
						// при переходе между блоками
						i.LastSymbol = readBuffer[findStringCount-1];

						if (FindFiles::IsWordDiv(i.LastSymbol))
							return true;

						// Если размер буфера меньше размера слова, то выходим
						if (readBlockSize < findStringCount)
						{
							ErrorState = true;
							continue;
						}
					}

					// Устанавливаем буфер сравнения
					buffer = readBuffer.data();
				}

				i.WordFound = false;
				unsigned int index = 0;

				do
				{
					// Ищем подстроку в буфере и возвращаем индекс её начала в случае успеха
					int foundIndex = FindStringBMH(buffer+index, bufferCount-index);

					// Если подстрока не найдена идём на следующий шаг
					if (foundIndex == -1)
						break;

					// Если подстрока найдена и отключен поиск по словам, то считаем что всё хорошо
					if (!WholeWords)
						return true;
					// Устанавливаем позицию в исходном буфере
					index += foundIndex;

					// Если идёт поиск по словам, то делаем соответствующие проверки
					bool firstWordDiv = false;

					// Если мы находимся вначале блока
					if (!index)
					{
						// Если мы находимся вначале файла, то считаем, что разделитель есть
						// Если мы находимся вначале блока, то проверяем является
						// или нет последний символ предыдущего блока разделителем
						if (alreadyRead == readBlockSize || FindFiles::IsWordDiv(i.LastSymbol))
							firstWordDiv = true;
					}
					else
					{
						// Проверяем является или нет предыдущий найденному символ блока разделителем
						i.LastSymbol = buffer[index-1];

						if (FindFiles::IsWordDiv(i.LastSymbol))
							firstWordDiv = true;
					}

					// Проверяем разделитель в конце, только если найден разделитель вначале
					if (firstWordDiv)
					{
						// Если блок выбран не до конца
						if (index+findStringCount!=bufferCount)
						{
							// Проверяем является или нет последующий за найденным символ блока разделителем
							i.LastSymbol = buffer[index+findStringCount];

							if (FindFiles::IsWordDiv(i.LastSymbol))
								return true;
						}
						else
							i.WordFound = true;
					}
				}
				while (++index<=bufferCount-findStringCount);

				// Выходим, если мы вышли за пределы количества байт разрешённых для поиска
				if (SearchInFirst && SearchInFirst>=alreadyRead)
				{
					ErrorState = true;
					continue;
				}
				// Запоминаем последний символ блока
				i.LastSymbol = buffer[bufferCount-1];
			}

			if (ErrorState)
				return false;

			// Получаем смещение на которое мы отступили при переходе между блоками
			offset = (CodePage == CP_SET? sizeof(wchar_t) : m_CodePages.begin()->MaxCharSize) * (findStringCount - 1);
		}

		// Если мы потенциально прочитали не весь файл
		if (readBlockSize == readBuffer.size())
		{
			// Отступаем назад на длину слова поиска минус 1
			if (!File.SetPointer(-1ll*offset, nullptr, FILE_CURRENT))
				return false;
			alreadyRead -= offset;
		}
	}

	return false;
}

bool background_searcher::IsFileIncluded(PluginPanelItem* FileItem, const string& FullName, DWORD FileAttr, const string &strDisplayName)
{
	if (!m_Owner->GetFileMask()->Compare(PointToName(FullName)))
		return false;

	const auto ArcItem = m_Owner->itd->GetFindFileArcItem();
	const auto hPlugin = ArcItem? ArcItem->hPlugin : nullptr;

	if (FileAttr & FILE_ATTRIBUTE_DIRECTORY)
		return Global->Opt->FindOpt.FindFolders && strFindStr.empty();

	m_Owner->itd->SetFindMessage(strDisplayName);

	string strSearchFileName;
	bool RemoveTemp = false;

	SCOPE_EXIT
	{
		if (RemoveTemp)
		DeleteFileWithFolder(strSearchFileName);
	};

	if (!hPlugin)
	{
		strSearchFileName = FullName;
	}
	else
	{
		const auto& UseFarCommand = [&]
		{
			SCOPED_ACTION(auto)(m_Owner->ScopedLock());
			return Global->CtrlObject->Plugins->UseFarCommand(hPlugin, PLUGIN_FARGETFILES);
		};

		if (UseFarCommand())
		{
			strSearchFileName = strPluginSearchPath + FullName;
		}
		else
		{
			string strTempDir;
			FarMkTempEx(strTempDir); // А проверка на nullptr???
			os::CreateDirectory(strTempDir,nullptr);

			const auto& GetFile = [&]
			{
				SCOPED_ACTION(auto)(m_Owner->ScopedLock());
				return Global->CtrlObject->Plugins->GetFile(hPlugin, FileItem, strTempDir, strSearchFileName, OPM_SILENT | OPM_FIND) != FALSE;
			};

			if (!GetFile())
			{
				os::RemoveDirectory(strTempDir);
				return false;
			}

			RemoveTemp = true;
		}
	}

	return LookForString(strSearchFileName) ^ NotContaining;
}

intptr_t FindFiles::FindDlgProc(Dialog* Dlg, intptr_t Msg, intptr_t Param1, void* Param2)
{
	if (!m_ExceptionPtr)
	{
		m_ExceptionPtr = m_Searcher->ExceptionPtr();
		if (m_ExceptionPtr)
		{
			Dlg->SendMessage(DM_CLOSE, 0, nullptr);
			return TRUE;
		}
	}

	auto& ListBox = Dlg->GetAllItem()[FD_LISTBOX].ListPtr;

	static int Recurse = 0;
	static int Drawing = 0;
	switch (Msg)
	{
	case DN_INITDIALOG:
		Drawing = 0;
		break;
	case DN_DRAWDIALOG:
		++Drawing;
		break;
	case DN_DRAWDIALOGDONE:
		--Drawing;
		break;
	default:
		if (!Finalized && !Recurse && !Drawing)
		{
			++Recurse;
			SCOPE_EXIT{ --Recurse; };
			{
				SCOPED_ACTION(auto)(m_Messages.scoped_lock());
				size_t EventsCount = 0;
				time_check TimeCheck(time_check::mode::delayed, GetRedrawTimeout());
				while (!m_Messages.empty() && 0 == EventsCount)
				{
					if (TimeCheck)
					{
						Global->WindowManager->CallbackWindow([](){ auto f = Global->WindowManager->GetCurrentWindow(); if (windowtype_dialog == f->GetType()) std::static_pointer_cast<Dialog>(f)->SendMessage(DN_ENTERIDLE, 0, nullptr); });
						break;
					}
					AddMenuData Data;
					if (m_Messages.try_pop(Data))
					{
						ProcessMessage(Data);
					}
					Console().GetNumberOfInputEvents(EventsCount);
				}
			}
		}
	}

	if(m_TimeCheck && !Finalized && !Recurse)
	{
		if (!m_Searcher->Stopped())
		{
			const auto strDataStr = format(lng::MFindFound, m_FileCount, m_DirCount);
			Dlg->SendMessage(DM_SETTEXTPTR,FD_SEPARATOR1, UNSAFE_CSTR(strDataStr));

			string strSearchStr;

			if (!strFindStr.empty())
			{
				string strFStr(strFindStr);
				TruncStrFromEnd(strFStr,10);
				strSearchStr = format(lng::MFindSearchingIn, concat(L'"', strFStr + L'"'));
			}

			string strFM;
			itd->GetFindMessage(strFM);
			SMALL_RECT Rect;
			Dlg->SendMessage(DM_GETITEMPOSITION, FD_TEXT_STATUS, &Rect);

			if (!strSearchStr.empty())
			{
				strSearchStr += L' ';
			}

			TruncStrFromCenter(strFM, Rect.Right-Rect.Left+1 - static_cast<int>(strSearchStr.size()));
			Dlg->SendMessage(DM_SETTEXTPTR, FD_TEXT_STATUS, UNSAFE_CSTR(strSearchStr + strFM));
			if (!strFindStr.empty())
			{
				Dlg->SendMessage(DM_SETTEXTPTR, FD_TEXT_STATUS_PERCENTS, UNSAFE_CSTR(format(L"{0:3}%", itd->GetPercent())));
			}

			if (m_LastFoundNumber)
			{
				m_LastFoundNumber = 0;

				if (ListBox->UpdateRequired())
					Dlg->SendMessage(DM_SHOWITEM,FD_LISTBOX,ToPtr(1));
			}
		}
	}

	if(!Recurse && !Finalized && m_Searcher->Stopped() && m_Messages.empty())
	{
		Finalized=true;
		const auto strMessage = format(lng::MFindDone, m_FileCount, m_DirCount);
		Dlg->SendMessage(DM_ENABLEREDRAW, FALSE, nullptr);
		Dlg->SendMessage( DM_SETTEXTPTR, FD_SEPARATOR1, nullptr);
		Dlg->SendMessage( DM_SETTEXTPTR, FD_TEXT_STATUS, UNSAFE_CSTR(strMessage));
		Dlg->SendMessage( DM_SETTEXTPTR, FD_TEXT_STATUS_PERCENTS, nullptr);
		Dlg->SendMessage( DM_SETTEXTPTR, FD_BUTTON_STOP, const_cast<wchar_t*>(msg(lng::MFindCancel)));
		Dlg->SendMessage(DM_ENABLEREDRAW, TRUE, nullptr);
		ConsoleTitle::SetFarTitle(strMessage);
		TB.reset();
	}

	switch (Msg)
	{
	case DN_INITDIALOG:
		{
			Dlg->GetAllItem()[FD_LISTBOX].ListPtr->SetMenuFlags(VMENU_NOMERGEBORDER);
		}
		break;

	case DN_DRAWDLGITEMDONE: //???
	case DN_DRAWDIALOGDONE:
		Dlg->DefProc(Msg,Param1,Param2);

		// Переместим фокус на кнопку [Go To]
		if ((m_DirCount || m_FileCount) && !FindPositionChanged)
		{
			FindPositionChanged=true;
			Dlg->SendMessage(DM_SETFOCUS, FD_BUTTON_GOTO, nullptr);
		}
		return TRUE;

	case DN_CONTROLINPUT:
		{
			const auto record = static_cast<const INPUT_RECORD*>(Param2);
			if (record->EventType!=KEY_EVENT) break;
			int key = InputRecordToKey(record);
			switch (key)
			{
			case KEY_ESC:
			case KEY_F10:
				{
					if (!m_Searcher->Stopped())
					{
						m_Searcher->Pause();
						bool LocalRes=true;
						if (Global->Opt->Confirm.Esc)
							LocalRes=ConfirmAbortOp()!=0;
						m_Searcher->Resume();
						if(LocalRes)
						{
							m_Searcher->Stop();
						}
						return TRUE;
					}
				}
				break;

			case KEY_ALTF9:
			case KEY_RALTF9:
			case KEY_F11:
			case KEY_CTRLW:
			case KEY_RCTRLW:
				Global->WindowManager->ProcessKey(Manager::Key(key));
				return TRUE;

			case KEY_RIGHT:
			case KEY_NUMPAD6:
			case KEY_TAB:
				if (Param1==FD_BUTTON_STOP)
				{
					FindPositionChanged=true;
					Dlg->SendMessage(DM_SETFOCUS, FD_BUTTON_NEW, nullptr);
					return TRUE;
				}
				break;

			case KEY_LEFT:
			case KEY_NUMPAD4:
			case KEY_SHIFTTAB:
				if (Param1==FD_BUTTON_NEW)
				{
					FindPositionChanged=true;
					Dlg->SendMessage(DM_SETFOCUS, FD_BUTTON_STOP, nullptr);
					return TRUE;
				}
				break;

			case KEY_UP:
			case KEY_DOWN:
			case KEY_NUMPAD8:
			case KEY_NUMPAD2:
			case KEY_PGUP:
			case KEY_PGDN:
			case KEY_NUMPAD9:
			case KEY_NUMPAD3:
			case KEY_HOME:
			case KEY_END:
			case KEY_NUMPAD7:
			case KEY_NUMPAD1:
			case KEY_MSWHEEL_UP:
			case KEY_MSWHEEL_DOWN:
			case KEY_ALTLEFT:
			case KEY_RALTLEFT:
			case KEY_ALT|KEY_NUMPAD4:
			case KEY_RALT|KEY_NUMPAD4:
			case KEY_MSWHEEL_LEFT:
			case KEY_ALTRIGHT:
			case KEY_RALTRIGHT:
			case KEY_ALT|KEY_NUMPAD6:
			case KEY_RALT|KEY_NUMPAD6:
			case KEY_MSWHEEL_RIGHT:
			case KEY_ALTSHIFTLEFT:
			case KEY_RALTSHIFTLEFT:
			case KEY_ALT|KEY_SHIFT|KEY_NUMPAD4:
			case KEY_RALT|KEY_SHIFT|KEY_NUMPAD4:
			case KEY_ALTSHIFTRIGHT:
			case KEY_RALTSHIFTRIGHT:
			case KEY_ALT|KEY_SHIFT|KEY_NUMPAD6:
			case KEY_RALT|KEY_SHIFT|KEY_NUMPAD6:
			case KEY_ALTHOME:
			case KEY_RALTHOME:
			case KEY_ALT|KEY_NUMPAD7:
			case KEY_RALT|KEY_NUMPAD7:
			case KEY_ALTEND:
			case KEY_RALTEND:
			case KEY_ALT|KEY_NUMPAD1:
			case KEY_RALT|KEY_NUMPAD1:
				ListBox->ProcessKey(Manager::Key(key));
				return TRUE;

			/*
			case KEY_CTRLA:
			case KEY_RCTRLA:
			{
				if (!ListBox->GetItemCount())
				{
					return TRUE;
				}

				size_t ItemIndex = *static_cast<size_t*>(ListBox->GetUserData(nullptr,0));

				FINDLIST FindItem;
				itd->GetFindListItem(ItemIndex, FindItem);

				if (ShellSetFileAttributes(nullptr,FindItem.FindData.strFileName))
				{
					itd->SetFindListItem(ItemIndex, FindItem);
					Dlg->SendMessage(DM_REDRAW,0,0);
				}
				return TRUE;
			}
			*/

			case KEY_F3:
			case KEY_ALTF3:
			case KEY_RALTF3:
			case KEY_CTRLSHIFTF3:
			case KEY_RCTRLSHIFTF3:
			case KEY_NUMPAD5:
			case KEY_SHIFTNUMPAD5:
			case KEY_F4:
			case KEY_ALTF4:
			case KEY_RALTF4:
			case KEY_CTRLSHIFTF4:
			case KEY_RCTRLSHIFTF4:
				{
					if (ListBox->empty())
					{
						return TRUE;
					}

					const auto FindItem = *ListBox->GetUserDataPtr<FindListItem*>();
					bool RemoveTemp=false;
					string strSearchFileName;
					string strTempDir;

					if (FindItem->FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					{
						return TRUE;
					}

					bool real_name = true;

					if(FindItem->Arc)
					{
						if(!(FindItem->Arc->Flags & OPIF_REALNAMES))
						{
							// Плагины надо закрывать, если открыли.
							bool ClosePanel=false;
							real_name = false;

							string strFindArcName = FindItem->Arc->strArcName;
							if(!FindItem->Arc->hPlugin)
							{
								const auto SavePluginsOutput = std::exchange(Global->DisablePluginsOutput, true);
								{
									SCOPED_ACTION(os::critical_section_lock)(PluginCS);
									FindItem->Arc->hPlugin = Global->CtrlObject->Plugins->OpenFilePlugin(&strFindArcName, OPM_NONE, OFP_SEARCH);
								}
								Global->DisablePluginsOutput=SavePluginsOutput;

								if (FindItem->Arc->hPlugin == PANEL_STOP ||
										!FindItem->Arc->hPlugin)
								{
									FindItem->Arc->hPlugin = nullptr;
									return TRUE;
								}

								ClosePanel = true;
							}
							FarMkTempEx(strTempDir);
							os::CreateDirectory(strTempDir, nullptr);
							UserDataItem UserData = {FindItem->Data,FindItem->FreeData};
							bool bGet=GetPluginFile(FindItem->Arc,FindItem->FindData,strTempDir,strSearchFileName,&UserData);
							if (!bGet)
							{
								os::RemoveDirectory(strTempDir);

								if (ClosePanel)
								{
									SCOPED_ACTION(os::critical_section_lock)(PluginCS);
									Global->CtrlObject->Plugins->ClosePanel(FindItem->Arc->hPlugin);
									FindItem->Arc->hPlugin = nullptr;
								}
								return FALSE;
							}
							else
							{
								if (ClosePanel)
								{
									SCOPED_ACTION(os::critical_section_lock)(PluginCS);
									Global->CtrlObject->Plugins->ClosePanel(FindItem->Arc->hPlugin);
									FindItem->Arc->hPlugin = nullptr;
								}
							}
							RemoveTemp=true;
						}
					}

					if (real_name)
					{
						strSearchFileName = FindItem->FindData.strFileName;
						if (!os::fs::exists(strSearchFileName) && os::fs::exists(FindItem->FindData.strAlternateFileName))
							strSearchFileName = FindItem->FindData.strAlternateFileName;
					}

					OpenFile(strSearchFileName, key, FindItem, Dlg);

					if (RemoveTemp)
					{
						// external editor may not have enough time to open this file, so defer deletion
						m_DelayedDeleters.emplace_back(strSearchFileName);
					}
					return TRUE;
				}

			default:
				break;
			}
		}
		break;

	case DN_BTNCLICK:
		{
			FindPositionChanged = true;
			switch (Param1)
			{
			case FD_BUTTON_NEW:
				m_Searcher->Stop();
				return FALSE;

			case FD_BUTTON_STOP:
				if(!m_Searcher->Stopped())
				{
					m_Searcher->Stop();
					return TRUE;
				}
				return FALSE;

			case FD_BUTTON_VIEW:
				{
					INPUT_RECORD key;
					KeyToInputRecord(KEY_F3,&key);
					FindDlgProc(Dlg,DN_CONTROLINPUT,FD_LISTBOX,&key);
					return TRUE;
				}

			case FD_BUTTON_GOTO:
			case FD_BUTTON_PANEL:
				// Переход и посыл на панель будем делать не в диалоге, а после окончания поиска.
				// Иначе возможна ситуация, когда мы ищем на панели, потом ее грохаем и создаем новую
				// (а поиск-то идет!) и в результате ФАР трапается.
				if(ListBox->empty())
				{
					return TRUE;
				}
				FindExitItem = *ListBox->GetUserDataPtr<FindListItem*>();
				TB.reset();
				return FALSE;

			default:
				break;
			}
		}
		break;

	case DN_CLOSE:
		{
			BOOL Result = TRUE;
			if (Param1==FD_LISTBOX)
			{
				if(!ListBox->empty())
				{
					FindDlgProc(Dlg, DN_BTNCLICK, FD_BUTTON_GOTO, nullptr); // emulates a [ Go to ] button pressing;
				}
				else
				{
					Result = FALSE;
				}
			}
			if(Result)
			{
				m_Searcher->Stop();
			}
			return Result;
		}

	case DN_RESIZECONSOLE:
		{
			const auto pCoord = static_cast<PCOORD>(Param2);
			SMALL_RECT DlgRect;
			Dlg->SendMessage( DM_GETDLGRECT, 0, &DlgRect);
			int DlgWidth=DlgRect.Right-DlgRect.Left+1;
			int DlgHeight=DlgRect.Bottom-DlgRect.Top+1;
			int IncX = pCoord->X - DlgWidth - 2;
			int IncY = pCoord->Y - DlgHeight - 2;
			Dlg->SendMessage(DM_ENABLEREDRAW, FALSE, nullptr);

			for (int i = 0; i <= FD_BUTTON_STOP; i++)
			{
				Dlg->SendMessage(DM_SHOWITEM, i, ToPtr(FALSE));
			}

			if ((IncX > 0) || (IncY > 0))
			{
				pCoord->X = DlgWidth + (IncX > 0 ? IncX : 0);
				pCoord->Y = DlgHeight + (IncY > 0 ? IncY : 0);
				Dlg->SendMessage( DM_RESIZEDIALOG, 0, pCoord);
			}

			DlgWidth += IncX;
			DlgHeight += IncY;

			for (int i = 0; i < FD_SEPARATOR1; i++)
			{
				SMALL_RECT rect;
				Dlg->SendMessage( DM_GETITEMPOSITION, i, &rect);
				rect.Right += IncX;
				rect.Bottom += IncY;
				Dlg->SendMessage( DM_SETITEMPOSITION, i, &rect);
			}

			for (int i = FD_SEPARATOR1; i <= FD_BUTTON_STOP; i++)
			{
				SMALL_RECT rect;
				Dlg->SendMessage( DM_GETITEMPOSITION, i, &rect);

				if (i == FD_TEXT_STATUS)
				{
					rect.Right += IncX;
				}
				else if (i==FD_TEXT_STATUS_PERCENTS)
				{
					rect.Right+=IncX;
					rect.Left+=IncX;
				}

				rect.Top += IncY;
				rect.Bottom += IncY;
				Dlg->SendMessage( DM_SETITEMPOSITION, i, &rect);
			}

			if ((IncX <= 0) || (IncY <= 0))
			{
				pCoord->X = DlgWidth;
				pCoord->Y = DlgHeight;
				Dlg->SendMessage( DM_RESIZEDIALOG, 0, pCoord);
			}

			for (int i = 0; i <= FD_BUTTON_STOP; i++)
			{
				Dlg->SendMessage( DM_SHOWITEM, i, ToPtr(TRUE));
			}

			Dlg->SendMessage(DM_ENABLEREDRAW, TRUE, nullptr);
			return TRUE;
		}

	default:
		break;
	}

	return Dlg->DefProc(Msg,Param1,Param2);
}

void FindFiles::OpenFile(string strSearchFileName, int openKey, const FindListItem* FindItem, Dialog* Dlg) const
{
	if (!os::fs::exists(strSearchFileName))
		return;

	auto openMode = FILETYPE_VIEW;
	auto shouldForceInternal = false;
	const auto isKnownKey = GetFiletypeOpenMode(openKey, openMode, shouldForceInternal);

	assert(isKnownKey); // ensure all possible keys are handled

	if (!isKnownKey)
		return;

	const auto strOldTitle = Console().GetTitle();
	const auto shortFileName = ExtractFileName(strSearchFileName);

	if (shouldForceInternal || !ProcessLocalFileTypes(strSearchFileName, shortFileName, openMode, PluginMode))
	{
		if (openMode == FILETYPE_ALTVIEW && Global->Opt->strExternalViewer.empty())
			openMode = FILETYPE_VIEW;

		if (openMode == FILETYPE_ALTEDIT && Global->Opt->strExternalEditor.empty())
			openMode = FILETYPE_EDIT;

		if (openMode == FILETYPE_VIEW)
		{
			NamesList ViewList;
			int list_count = 0;

			// Возьмем все файлы, которые имеют реальные имена...
			itd->ForEachFindItem([&list_count, &ViewList](const FindListItem& i)
			{
				if (!i.Arc || (i.Arc->Flags & OPIF_REALNAMES))
				{
					if (!i.FindData.strFileName.empty() && !(i.FindData.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
					{
						++list_count;
						ViewList.AddName(i.FindData.strFileName);
					}
				}
			});

			ViewList.SetCurName(FindItem->FindData.strFileName);

			Dlg->SendMessage(DM_SHOWDIALOG, FALSE, nullptr);
			Dlg->SendMessage(DM_ENABLEREDRAW, FALSE, nullptr);
			{
				const auto ShellViewer = FileViewer::create(strSearchFileName, false, false, false, -1, nullptr, (list_count > 1? &ViewList : nullptr));
				ShellViewer->SetEnableF6(TRUE);

				if (FindItem->Arc && !(FindItem->Arc->Flags & OPIF_REALNAMES))
					ShellViewer->SetSaveToSaveAs(true);

				Global->WindowManager->ExecuteModal(ShellViewer);
				// заставляем рефрешится экран
				Global->WindowManager->ResizeAllWindows();
			}
			Dlg->SendMessage(DM_ENABLEREDRAW, TRUE, nullptr);
			Dlg->SendMessage(DM_SHOWDIALOG, TRUE, nullptr);
		}

		if (openMode == FILETYPE_EDIT)
		{
			const auto ShellEditor = FileEditor::create(strSearchFileName, CP_DEFAULT, 0);
			ShellEditor->SetEnableF6(true);

			if (FindItem->Arc && !(FindItem->Arc->Flags & OPIF_REALNAMES))
				ShellEditor->SetSaveToSaveAs(true);

			const auto editorExitCode = ShellEditor->GetExitCode();
			if (editorExitCode != XC_OPEN_ERROR && editorExitCode != XC_LOADING_INTERRUPTED)
			{
				Global->WindowManager->ExecuteModal(ShellEditor);
				// заставляем рефрешится экран
				Global->WindowManager->ResizeAllWindows();
			}
		}

		if (openMode == FILETYPE_ALTEDIT || openMode == FILETYPE_ALTVIEW)
		{
			const auto& externalCommand = openMode == FILETYPE_ALTEDIT? Global->Opt->strExternalEditor : Global->Opt->strExternalViewer;
			ProcessExternal(externalCommand, strSearchFileName, shortFileName, PluginMode);
		}
	}

	Console().SetTitle(strOldTitle);
}

void FindFiles::AddMenuRecord(Dialog* Dlg,const string& FullName, const os::FAR_FIND_DATA& FindData, void* Data, FARPANELITEMFREECALLBACK FreeData, ArcListItem* Arc)
{
	if (!Dlg)
		return;

	auto& ListBox = Dlg->GetAllItem()[FD_LISTBOX].ListPtr;

	if(ListBox->empty())
	{
		Dlg->SendMessage( DM_ENABLE, FD_BUTTON_GOTO, ToPtr(TRUE));
		Dlg->SendMessage( DM_ENABLE, FD_BUTTON_VIEW, ToPtr(TRUE));
		if(AnySetFindList)
		{
			Dlg->SendMessage( DM_ENABLE, FD_BUTTON_PANEL, ToPtr(TRUE));
		}
		Dlg->SendMessage( DM_ENABLE, FD_LISTBOX, ToPtr(TRUE));
	}

	const wchar_t *DisplayName=FindData.strFileName.data();

	string MenuText(1, L' ');

	for (auto& i: Global->Opt->FindOpt.OutColumns)
	{
		int CurColumnType = static_cast<int>(i.type & 0xFF);
		int Width = i.width;
		if (!Width)
		{
			Width = GetDefaultWidth(i.type);
		}

		switch (CurColumnType)
		{
			case DIZ_COLUMN:
			case OWNER_COLUMN:
			{
				// пропускаем, не реализовано
				break;
			}
			case NAME_COLUMN:
			{
				// даже если указали, пропускаем, т.к. поле имени обязательное и идет в конце.
				break;
			}

			case ATTR_COLUMN:
			{
				append(MenuText, FormatStr_Attribute(FindData.dwFileAttributes, Width), BoxSymbols[BS_V1]);
				break;
			}
			case NUMSTREAMS_COLUMN:
			case STREAMSSIZE_COLUMN:
			case SIZE_COLUMN:
			case PACKED_COLUMN:
			case NUMLINK_COLUMN:
			{
				unsigned long long StreamsSize = 0;
				DWORD StreamsCount=0;

				if (Arc)
				{
					if (CurColumnType == NUMSTREAMS_COLUMN || CurColumnType == STREAMSSIZE_COLUMN)
						EnumStreams(FindData.strFileName,StreamsSize,StreamsCount);
					else if(CurColumnType == NUMLINK_COLUMN)
						StreamsCount=GetNumberOfLinks(FindData.strFileName);
				}

				const auto SizeToDisplay = (CurColumnType == SIZE_COLUMN)
					? FindData.nFileSize
					: (CurColumnType == PACKED_COLUMN)
					? FindData.nAllocationSize
					: (CurColumnType == STREAMSSIZE_COLUMN)
					? StreamsSize
					: StreamsCount; // ???

				append(MenuText, FormatStr_Size(
								SizeToDisplay,
								DisplayName,
								FindData.dwFileAttributes,
								0,
								FindData.dwReserved0,
								(CurColumnType == NUMSTREAMS_COLUMN || CurColumnType == NUMLINK_COLUMN)?STREAMSSIZE_COLUMN:CurColumnType,
								i.type,
								Width), BoxSymbols[BS_V1]);
				break;
			}

			case DATE_COLUMN:
			case TIME_COLUMN:
			case WDATE_COLUMN:
			case ADATE_COLUMN:
			case CDATE_COLUMN:
			case CHDATE_COLUMN:
			{
				const FILETIME *FileTime;
				switch (CurColumnType)
				{
					case CDATE_COLUMN:
						FileTime=&FindData.ftCreationTime;
						break;
					case ADATE_COLUMN:
						FileTime=&FindData.ftLastAccessTime;
						break;
					case CHDATE_COLUMN:
						FileTime=&FindData.ftChangeTime;
						break;
					case DATE_COLUMN:
					case TIME_COLUMN:
					case WDATE_COLUMN:
					default:
						FileTime=&FindData.ftLastWriteTime;
						break;
				}

				append(MenuText, FormatStr_DateTime(FileTime, CurColumnType, i.type, Width), BoxSymbols[BS_V1]);
				break;
			}
		}
	}


	// В плагинах принудительно поставим указатель в имени на имя
	// для корректного его отображения в списке, отбросив путь,
	// т.к. некоторые плагины возвращают имя вместе с полным путём,
	// к примеру временная панель.

	const wchar_t *DisplayName0=DisplayName;
	if (Arc)
		DisplayName0 = PointToName(DisplayName0);
	MenuText += DisplayName0;

	string strPathName=FullName;
	{
		const auto pos = FindLastSlash(strPathName);
		if (pos != string::npos)
		{
			strPathName.resize(pos);
		}
		else
		{
			strPathName.clear();
	}
	}
	AddEndSlash(strPathName);

	if (StrCmpI(strPathName, m_LastDirName))
	{
		if (!ListBox->empty())
		{
			MenuItemEx ListItem;
			ListItem.Flags|=LIF_SEPARATOR;
			ListBox->AddItem(std::move(ListItem));
		}

		m_LastDirName = strPathName;

		if (const auto ArcItem = Arc)
		{
			if(!(ArcItem->Flags & OPIF_REALNAMES) && !ArcItem->strArcName.empty())
			{
				auto strArcPathName = ArcItem->strArcName + L':';

				if (!IsSlash(strPathName.front()))
					AddEndSlash(strArcPathName);

				strArcPathName += strPathName == L".\\"? L"\\" : strPathName;
				strPathName = strArcPathName;
			}
		}
		FindListItem& FindItem = itd->AddFindListItem(FindData,Data,nullptr);
		// Сбросим данные в FindData. Они там от файла
		FindItem.FindData = {};
		// Используем LastDirName, т.к. PathName уже может быть искажена
		FindItem.FindData.strFileName = m_LastDirName;
		// Used=0 - Имя не попадёт во временную панель.
		FindItem.Used=0;
		// Поставим атрибут у каталога, чтобы он не был файлом :)
		FindItem.FindData.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
		FindItem.Arc = Arc;;

		const auto Ptr = &FindItem;
		MenuItemEx ListItem(strPathName);
		ListItem.UserData = Ptr;
		ListBox->AddItem(std::move(ListItem));
	}

	FindListItem& FindItem = itd->AddFindListItem(FindData,Data,FreeData);
	FindItem.FindData.strFileName = FullName;
	FindItem.Used=1;
	FindItem.Arc = Arc;

	int ListPos;
	{
	MenuItemEx ListItem(MenuText);
	ListItem.UserData = &FindItem;
		ListPos = ListBox->AddItem(std::move(ListItem));
	}

	// Выделим как положено - в списке.
	if (!m_FileCount && !m_DirCount)
	{
		ListBox->SetSelectPos(ListPos, -1);
	}

	if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	{
		++m_DirCount;
	}
	else
	{
		++m_FileCount;
	}

	++m_LastFoundNumber;
}

void background_searcher::AddMenuRecord(const string& FullName, PluginPanelItem& FindData) const
{
	os::FAR_FIND_DATA fdata;
	PluginPanelItemToFindDataEx(FindData, fdata);
	m_Owner->m_Messages.emplace(FullName, fdata, FindData.UserData.Data, FindData.UserData.FreeData, m_Owner->itd->GetFindFileArcItem());
	FindData.UserData.FreeData = nullptr; //передано в FINDLIST
}

void background_searcher::ArchiveSearch(const string& ArcName)
{
	_ALGO(CleverSysLog clv(L"FindFiles::ArchiveSearch()"));
	_ALGO(SysLog(L"ArcName='%s'",(ArcName?ArcName:L"nullptr")));

	plugin_panel* hArc;
	{
		const auto SavePluginsOutput = std::exchange(Global->DisablePluginsOutput, true);

		string strArcName = ArcName;
		{
			SCOPED_ACTION(auto)(m_Owner->ScopedLock());
			hArc = Global->CtrlObject->Plugins->OpenFilePlugin(&strArcName, OPM_FIND, OFP_SEARCH);
		}
		Global->DisablePluginsOutput = SavePluginsOutput;
	}
	if (hArc==PANEL_STOP)
	{
		//StopEvent.Set(); // ??
		_ALGO(SysLog(L"return: hArc==(HANDLE)-2"));
		return;
	}

	if (!hArc)
	{
		_ALGO(SysLog(L"return: hArc==nullptr"));
		return;
	}

	FINDAREA SaveSearchMode = SearchMode;
	FindFiles::ArcListItem* SaveArcItem = m_Owner->itd->GetFindFileArcItem();
	{
		const auto SavePluginsOutput = std::exchange(Global->DisablePluginsOutput, true);

		// BUGBUG
		const_cast<FINDAREA&>(SearchMode) = FINDAREA_FROM_CURRENT;
		OpenPanelInfo Info;
		{
			SCOPED_ACTION(auto)(m_Owner->ScopedLock());
			Global->CtrlObject->Plugins->GetOpenPanelInfo(hArc, &Info);
		}
		m_Owner->itd->SetFindFileArcItem(&m_Owner->itd->AddArcListItem(ArcName, hArc, Info.Flags, NullToEmpty(Info.CurDir)));
		// Запомним каталог перед поиском в архиве. И если ничего не нашли - не рисуем его снова.
		{
			string strSaveSearchPath;
			// Запомним пути поиска в плагине, они могут измениться.
			strSaveSearchPath = strPluginSearchPath;
			m_Owner->m_Messages.emplace(FindFiles::push);
			DoPreparePluginList(true);
			strPluginSearchPath = strSaveSearchPath;
			FindFiles::ArcListItem* ArcItem = m_Owner->itd->GetFindFileArcItem();
			{
				SCOPED_ACTION(auto)(m_Owner->ScopedLock());
				Global->CtrlObject->Plugins->ClosePanel(ArcItem->hPlugin);
			}
			ArcItem->hPlugin = nullptr;

			m_Owner->m_Messages.emplace(FindFiles::pop);
		}

		Global->DisablePluginsOutput=SavePluginsOutput;
	}
	m_Owner->itd->SetFindFileArcItem(SaveArcItem);
	// BUGBUG
	const_cast<FINDAREA&>(SearchMode) = SaveSearchMode;
}

void background_searcher::DoScanTree(const string& strRoot)
{
	ScanTree ScTree(
		false,
		!(SearchMode==FINDAREA_CURRENT_ONLY||SearchMode==FINDAREA_INPATH),
		Global->Opt->FindOpt.FindSymLinks
	);
	string strSelName;
	DWORD FileAttr;

	if (SearchMode==FINDAREA_SELECTED)
		Global->CtrlObject->Cp()->ActivePanel()->GetSelName(nullptr,FileAttr);

	while (!Stopped())
	{
		string strCurRoot;

		if (SearchMode==FINDAREA_SELECTED)
		{
			if (!Global->CtrlObject->Cp()->ActivePanel()->GetSelName(&strSelName,FileAttr))
				break;

			if (!(FileAttr & FILE_ATTRIBUTE_DIRECTORY) || TestParentFolderName(strSelName) || strSelName == L".")
				continue;

			strCurRoot = strRoot;
			AddEndSlash(strCurRoot);
			strCurRoot += strSelName;
		}
		else
		{
			strCurRoot = strRoot;
		}

		ScTree.SetFindPath(strCurRoot,L"*");
		m_Owner->itd->SetFindMessage(strCurRoot);
		os::FAR_FIND_DATA FindData;
		string strFullName;

		while (!Stopped() && ScTree.GetNextName(FindData, strFullName))
		{
			os::find_file_handle FindStream;

			Sleep(0);
			PauseEvent.wait();

			bool bContinue=false;
			WIN32_FIND_STREAM_DATA sd;
			bool FirstCall=true;
			string strFindDataFileName=FindData.strFileName;

			if (Global->Opt->FindOpt.FindAlternateStreams)
			{
				FindStream = os::FindFirstStream(strFullName, FindStreamInfoStandard, &sd);
			}

			// process default streams first
			bool ProcessAlternateStreams = false;
			while (!Stopped())
			{
				string strFullStreamName=strFullName;

				if (ProcessAlternateStreams)
				{
					if (FindStream)
					{
						if (!FirstCall)
						{
							if (!os::FindNextStream(FindStream, &sd))
							{
								break;
							}
						}
						else
						{
							FirstCall=false;
						}

						LPWSTR NameEnd=wcschr(sd.cStreamName+1,L':');

						if (NameEnd)
						{
							*NameEnd=L'\0';
						}

						if (sd.cStreamName[1]) // alternate stream
						{
							strFullStreamName+=sd.cStreamName;
							FindData.strFileName=strFindDataFileName+sd.cStreamName;
							FindData.nFileSize=sd.StreamSize.QuadPart;
							FindData.dwFileAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;
						}
						else
						{
							// default stream is already processed
							continue;
						}
					}
					else
					{
						if (bContinue)
						{
							break;
						}
					}
				}

				if (UseFilter)
				{
					enumFileInFilterType foundType;

					if (!m_Owner->GetFilter()->FileInFilter(FindData, &foundType, &strFullName))
					{
						// сюда заходим, если не попали в фильтр или попали в Exclude-фильтр
						if ((FindData.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && foundType==FIFT_EXCLUDE)
							ScTree.SkipDir(); // скипаем только по Exclude-фильтру, т.к. глубже тоже нужно просмотреть

						{
							bContinue=true;

							if (ProcessAlternateStreams)
							{
								continue;
							}
							else
							{
								break;
							}
						}
					}
				}

				if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					m_Owner->itd->SetFindMessage(strFullName);
				}

				if (IsFileIncluded(nullptr,strFullStreamName,FindData.dwFileAttributes,strFullName))
				{
					m_Owner->m_Messages.emplace(strFullStreamName, FindData, nullptr, nullptr, nullptr);
				}

				ProcessAlternateStreams = Global->Opt->FindOpt.FindAlternateStreams;
				if (!Global->Opt->FindOpt.FindAlternateStreams || !FindStream)
				{
					break;
				}
			}

			if (bContinue)
			{
				continue;
			}

			if (SearchInArchives)
				ArchiveSearch(strFullName);
		}

		if (SearchMode!=FINDAREA_SELECTED)
			break;
	}
}

void background_searcher::ScanPluginTree(plugin_panel* hPlugin, unsigned long long Flags, int& RecurseLevel)
{
	PluginPanelItem *PanelData=nullptr;
	size_t ItemCount=0;
	bool GetFindDataResult=false;

	if(!Stopped())
	{
		SCOPED_ACTION(auto)(m_Owner->ScopedLock());
		GetFindDataResult = Global->CtrlObject->Plugins->GetFindData(hPlugin, &PanelData, &ItemCount, OPM_FIND) != FALSE;
	}

	if (!GetFindDataResult)
	{
		return;
	}

	RecurseLevel++;

	if (SearchMode!=FINDAREA_SELECTED || RecurseLevel!=1)
	{
		for (size_t I=0; I<ItemCount && !Stopped(); I++)
		{
			Sleep(0);
			PauseEvent.wait();

			PluginPanelItem *CurPanelItem=PanelData+I;
			string strCurName=NullToEmpty(CurPanelItem->FileName);
			if (strCurName.empty() || strCurName == L"." || TestParentFolderName(strCurName))
				continue;

			string strFullName = strPluginSearchPath;
			strFullName += strCurName;

			if (!UseFilter || m_Owner->GetFilter()->FileInFilter(*CurPanelItem))
			{
				if (CurPanelItem->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					m_Owner->itd->SetFindMessage(strFullName);
				}

				if (IsFileIncluded(CurPanelItem,strCurName,CurPanelItem->FileAttributes,strFullName))
					AddMenuRecord(strFullName, *CurPanelItem);

				if (SearchInArchives && hPlugin && (Flags & OPIF_REALNAMES))
					ArchiveSearch(strFullName);
			}
		}
	}

	if (SearchMode!=FINDAREA_CURRENT_ONLY)
	{
		for (size_t I = 0; I<ItemCount && !Stopped(); I++)
		{
			PluginPanelItem *CurPanelItem=PanelData+I;

			if ((CurPanelItem->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			 && StrCmp(CurPanelItem->FileName, L".") && !TestParentFolderName(CurPanelItem->FileName)
			 && (!UseFilter || m_Owner->GetFilter()->FileInFilter(*CurPanelItem))
			 && (SearchMode!=FINDAREA_SELECTED || RecurseLevel!=1 || Global->CtrlObject->Cp()->ActivePanel()->IsSelected(CurPanelItem->FileName))
			)
			{
				bool SetDirectoryResult;
				{
					SCOPED_ACTION(auto)(m_Owner->ScopedLock());
					SetDirectoryResult=Global->CtrlObject->Plugins->SetDirectory(hPlugin, CurPanelItem->FileName, OPM_FIND, &CurPanelItem->UserData)!=FALSE;
				}
				if (SetDirectoryResult && CurPanelItem->FileName[0])
				{
					strPluginSearchPath += CurPanelItem->FileName;
					strPluginSearchPath += L'\\';
					ScanPluginTree(hPlugin, Flags, RecurseLevel);

					size_t pos = strPluginSearchPath.rfind(L'\\');
					if (pos != string::npos)
						strPluginSearchPath.resize(pos);

					if ((pos = strPluginSearchPath.rfind(L'\\')) != string::npos)
						strPluginSearchPath.resize(pos+1);
					else
						strPluginSearchPath.clear();

					{
						SCOPED_ACTION(auto)(m_Owner->ScopedLock());
						SetDirectoryResult=Global->CtrlObject->Plugins->SetDirectory(hPlugin,L"..",OPM_FIND)!=FALSE;
					}

					if (!SetDirectoryResult)
					{
						// BUGBUG, better way to stop searcher?
						Stop();
					}
				}
			}
		}
	}

	{
		SCOPED_ACTION(auto)(m_Owner->ScopedLock());
		Global->CtrlObject->Plugins->FreeFindData(hPlugin, PanelData, ItemCount, true);
	}
	RecurseLevel--;
}

void background_searcher::DoPrepareFileList()
{
	const auto& GetCurDir = []
	{
		auto CurDir = Global->CtrlObject->CmdLine()->GetCurDir();
		if (CurDir.find_first_of(L";,") != string::npos)
			InsertQuote(CurDir);
		return CurDir;
	};

	string InitString;

	if (SearchMode==FINDAREA_INPATH)
	{
		InitString = os::env::get_variable(L"PATH");
	}
	else if (SearchMode==FINDAREA_ROOT)
	{
		InitString = GetPathRoot(GetCurDir());
	}
	else if (SearchMode==FINDAREA_ALL || SearchMode==FINDAREA_ALL_BUTNETWORK)
	{
		const auto Drives = os::fs::get_logical_drives();
		std::vector<string> Volumes;
		Volumes.reserve(Drives.count());

		for (const auto& i: os::fs::enum_drives(Drives))
		{
			const auto RootDir = os::fs::get_root_directory(i);

			int DriveType=FAR_GetDriveType(RootDir);

			if (DriveType != DRIVE_REMOVABLE && !IsDriveTypeCDROM(DriveType) && (DriveType != DRIVE_REMOTE || SearchMode != FINDAREA_ALL_BUTNETWORK))
			{
				string strGuidVolime;
				if(os::GetVolumeNameForVolumeMountPoint(RootDir, strGuidVolime))
				{
					Volumes.emplace_back(std::move(strGuidVolime));
				}
				append(InitString, RootDir, L';');
			}
		}

		for (const auto& VolumeName: os::fs::enum_volumes())
		{
			int DriveType=FAR_GetDriveType(VolumeName);

			if (DriveType==DRIVE_REMOVABLE || IsDriveTypeCDROM(DriveType) || (DriveType==DRIVE_REMOTE && SearchMode==FINDAREA_ALL_BUTNETWORK))
			{
				continue;
			}

			if (std::none_of(CONST_RANGE(Volumes, i) {return i.compare(0, VolumeName.size(), VolumeName) == 0;}))
			{
				append(InitString, VolumeName, L';');
			}
		}
	}
	else
	{
		InitString = GetCurDir();
	}

	for (const auto& i: split<std::vector<string>>(InitString, STLF_UNIQUE))
	{
		DoScanTree(i);
	}

	m_Owner->itd->SetPercent(0);
	// BUGBUG, better way to stop searcher?
	Stop();
}

void background_searcher::DoPreparePluginList(bool Internal)
{
	FindFiles::ArcListItem* ArcItem = m_Owner->itd->GetFindFileArcItem();
	OpenPanelInfo Info;
	string strSaveDir;
	{
		SCOPED_ACTION(auto)(m_Owner->ScopedLock());
		Global->CtrlObject->Plugins->GetOpenPanelInfo(ArcItem->hPlugin,&Info);
		strSaveDir = NullToEmpty(Info.CurDir);
		if (SearchMode==FINDAREA_ROOT || SearchMode==FINDAREA_ALL || SearchMode==FINDAREA_ALL_BUTNETWORK || SearchMode==FINDAREA_INPATH)
		{
			Global->CtrlObject->Plugins->SetDirectory(ArcItem->hPlugin,L"\\",OPM_FIND);
			Global->CtrlObject->Plugins->GetOpenPanelInfo(ArcItem->hPlugin,&Info);
		}
	}

	strPluginSearchPath = NullToEmpty(Info.CurDir);

	if (!strPluginSearchPath.empty())
		AddEndSlash(strPluginSearchPath);

	int RecurseLevel=0;
	ScanPluginTree(ArcItem->hPlugin, ArcItem->Flags, RecurseLevel);

	if (SearchMode==FINDAREA_ROOT || SearchMode==FINDAREA_ALL || SearchMode==FINDAREA_ALL_BUTNETWORK || SearchMode==FINDAREA_INPATH)
	{
		SCOPED_ACTION(auto)(m_Owner->ScopedLock());
		Global->CtrlObject->Plugins->SetDirectory(ArcItem->hPlugin,strSaveDir,OPM_FIND,&Info.UserData);
	}

	if (!Internal)
	{
		m_Owner->itd->SetPercent(0);
		// BUGBUG, better way to stop searcher?
		Stop();
	}
}

struct THREADPARAM
{
	bool PluginMode;
};

void background_searcher::Search()
{
	seh_invoke_thread(m_ExceptionPtr, [this]
	{
		try
		{
			SCOPED_ACTION(wakeful);
			InitInFileSearch();
			m_PluginMode? DoPreparePluginList(false) : DoPrepareFileList();
			ReleaseInFileSearch();
		}
		catch (...)
		{
			m_ExceptionPtr = std::current_exception();
			m_IsRegularException = true;
		}
	});
}

void background_searcher::Stop() const
{
	StopEvent.set();
}

bool FindFiles::FindFilesProcess()
{
	_ALGO(CleverSysLog clv(L"FindFiles::FindFilesProcess()"));
	// Если используется фильтр операций, то во время поиска сообщаем об этом
	string strTitle=msg(lng::MFindFileTitle);

	itd->Init();

	m_FileCount = 0;
	m_DirCount = 0;
	m_LastFoundNumber = 0;

	if (!strFindMask.empty())
	{
		append(strTitle, L": ", strFindMask);

		if (UseFilter)
		{
			append(strTitle, L" (", msg(lng::MFindUsingFilter), L')');
		}
	}
	else
	{
		if (UseFilter)
		{
			append(strTitle, L" (", msg(lng::MFindUsingFilter), L')');
		}
	}

	int DlgWidth = ScrX + 1 - 2;
	int DlgHeight = ScrY + 1 - 2;
	FarDialogItem FindDlgData[]=
	{
		{DI_DOUBLEBOX,3,1,DlgWidth-4,DlgHeight-2,0,nullptr,nullptr,DIF_SHOWAMPERSAND,strTitle.data()},
		{DI_LISTBOX,4,2,DlgWidth-5,DlgHeight-7,0,nullptr,nullptr,DIF_LISTNOBOX|DIF_DISABLE,L""},
		{DI_TEXT,-1,DlgHeight-6,0,DlgHeight-6,0,nullptr,nullptr,DIF_SEPARATOR2,L""},
		{DI_TEXT,5,DlgHeight-5,DlgWidth-(strFindStr.empty()?6:12),DlgHeight-5,0,nullptr,nullptr,DIF_SHOWAMPERSAND,L"..."},
		{DI_TEXT,DlgWidth-9,DlgHeight-5,DlgWidth-6,DlgHeight-5,0,nullptr,nullptr,(strFindStr.empty()?DIF_HIDDEN:0),L""},
		{DI_TEXT,-1,DlgHeight-4,0,DlgHeight-4,0,nullptr,nullptr,DIF_SEPARATOR,L""},
		{DI_BUTTON,0,DlgHeight-3,0,DlgHeight-3,0,nullptr,nullptr,DIF_FOCUS|DIF_DEFAULTBUTTON|DIF_CENTERGROUP,msg(lng::MFindNewSearch)},
		{DI_BUTTON,0,DlgHeight-3,0,DlgHeight-3,0,nullptr,nullptr,DIF_CENTERGROUP|DIF_DISABLE,msg(lng::MFindGoTo)},
		{DI_BUTTON,0,DlgHeight-3,0,DlgHeight-3,0,nullptr,nullptr,DIF_CENTERGROUP|DIF_DISABLE,msg(lng::MFindView)},
		{DI_BUTTON,0,DlgHeight-3,0,DlgHeight-3,0,nullptr,nullptr,DIF_CENTERGROUP|DIF_DISABLE,msg(lng::MFindPanel)},
		{DI_BUTTON,0,DlgHeight-3,0,DlgHeight-3,0,nullptr,nullptr,DIF_CENTERGROUP,msg(lng::MFindStop)},
	};
	auto FindDlg = MakeDialogItemsEx(FindDlgData);
	SCOPED_ACTION(ChangePriority)(THREAD_PRIORITY_NORMAL);

	if (PluginMode)
	{
		const auto hPlugin = Global->CtrlObject->Cp()->ActivePanel()->GetPluginHandle();
		OpenPanelInfo Info;
		// no lock - background thread hasn't been started yet
		Global->CtrlObject->Plugins->GetOpenPanelInfo(hPlugin,&Info);
		itd->SetFindFileArcItem(&itd->AddArcListItem(NullToEmpty(Info.HostFile), hPlugin, Info.Flags, NullToEmpty(Info.CurDir)));

		if (!(Info.Flags & OPIF_REALNAMES))
		{
			FindDlg[FD_BUTTON_PANEL].Type=DI_TEXT;
			FindDlg[FD_BUTTON_PANEL].strData.clear();
		}
	}

	AnySetFindList = std::any_of(CONST_RANGE(*Global->CtrlObject->Plugins, i)
	{
		return i->has(iSetFindList);
	});

	if (!AnySetFindList)
	{
		FindDlg[FD_BUTTON_PANEL].Flags|=DIF_DISABLE;
	}

	const auto Dlg = Dialog::create(FindDlg, &FindFiles::FindDlgProc, this);
	Dlg->SetHelp(L"FindFileResult");
	Dlg->SetPosition(-1, -1, DlgWidth, DlgHeight);
	Dlg->SetId(FindFileResultId);

	m_ResultsDialogPtr = Dlg.get();

		TB = std::make_unique<IndeterminateTaskBar>();

		m_Messages.clear();

		{
			background_searcher BC(this, strFindStr, SearchMode, CodePage, SearchInFirst, CmpCase, WholeWords, SearchInArchives, SearchHex, NotContaining, UseFilter, PluginMode);

			// BUGBUG
			m_Searcher = &BC;

			m_TimeCheck.reset();

			// Надо бы показать диалог, а то инициализация элементов запаздывает
			// иногда при поиске и первые элементы не добавляются
			Dlg->InitDialog();
			Dlg->Show();

			os::thread FindThread(&os::thread::join, &background_searcher::Search, &BC);

			// In case of an exception in the main thread
			SCOPE_EXIT
			{
				Dlg->CloseDialog();
				m_Searcher->Stop();
				m_Searcher = nullptr;
			};

			Dlg->Process();

			if (!m_ExceptionPtr)
			{
				m_ExceptionPtr = BC.ExceptionPtr();
			}

			if (m_ExceptionPtr && !BC.IsRegularException())
			{
				// You're someone else's problem
				FindThread.detach();
			}

			RethrowIfNeeded(m_ExceptionPtr);
		}

		switch (Dlg->GetExitCode())
		{
			case FD_BUTTON_NEW:
			{
				return true;
			}

			case FD_BUTTON_PANEL:
			// Отработаем переброску на временную панель
			{
				std::vector<PluginPanelItem> PanelItems;
				PanelItems.reserve(itd->GetFindListCount());

				itd->ForEachFindItem([&PanelItems, this](FindListItem& i)
				{
					if (!i.FindData.strFileName.empty() && i.Used)
					{
					// Добавляем всегда, если имя задано
						// Для плагинов с виртуальными именами заменим имя файла на имя архива.
						// панель сама уберет лишние дубли.
						const auto IsArchive = i.Arc && !(i.Arc->Flags&OPIF_REALNAMES);
						// Добавляем только файлы или имена архивов или папки когда просили
						if (IsArchive || (Global->Opt->FindOpt.FindFolders && !SearchHex) ||
							    !(i.FindData.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
						{
							if (IsArchive)
							{
								i.FindData.strFileName = i.Arc->strArcName;
							}
							PluginPanelItemHolderNonOwning pi;
							FindDataExToPluginPanelItemHolder(i.FindData, pi);

							if (IsArchive)
								pi.Item.FileAttributes = 0;

							if (pi.Item.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
							{
								DeleteEndSlash(const_cast<wchar_t*>(pi.Item.FileName));
							}
							PanelItems.emplace_back(pi.Item);
						}
					}
				});

				SCOPED_ACTION(os::critical_section_lock)(PluginCS);
				{
					if (const auto hNewPlugin = Global->CtrlObject->Plugins->OpenFindListPlugin(PanelItems.data(), PanelItems.size()))
					{
						const auto NewPanel = Global->CtrlObject->Cp()->ChangePanel(Global->CtrlObject->Cp()->ActivePanel(), panel_type::FILE_PANEL, TRUE, TRUE);
						NewPanel->SetPluginMode(hNewPlugin, L"", true);
						NewPanel->SetVisible(true);
						NewPanel->Update(0);
						//if (FindExitItem)
						//NewPanel->GoToFile(FindExitItem->FindData.cFileName);
						NewPanel->Show();
					}
				}

				FreePluginPanelItems(PanelItems);
				break;
			}
			case FD_BUTTON_GOTO:
			case FD_LISTBOX:
			{
				string strFileName=FindExitItem->FindData.strFileName;
				auto FindPanel = Global->CtrlObject->Cp()->ActivePanel();

				if (FindExitItem->Arc)
				{
					if (!FindExitItem->Arc->hPlugin)
					{
						string strArcName = FindExitItem->Arc->strArcName;

						if (FindPanel->GetType() != panel_type::FILE_PANEL)
						{
							FindPanel = Global->CtrlObject->Cp()->ChangePanel(FindPanel, panel_type::FILE_PANEL, TRUE, TRUE);
						}

						string strArcPath=strArcName;
						CutToSlash(strArcPath);
						FindPanel->SetCurDir(strArcPath,true);
						FindExitItem->Arc->hPlugin = std::static_pointer_cast<FileList>(FindPanel)->OpenFilePlugin(strArcName, FALSE, OFP_SEARCH);
						if (FindExitItem->Arc->hPlugin==PANEL_STOP)
							FindExitItem->Arc->hPlugin = nullptr;
					}

					if (FindExitItem->Arc->hPlugin)
					{
						OpenPanelInfo Info;
						{
							SCOPED_ACTION(os::critical_section_lock)(PluginCS);
							Global->CtrlObject->Plugins->GetOpenPanelInfo(FindExitItem->Arc->hPlugin, &Info);

							if (SearchMode == FINDAREA_ROOT ||
								SearchMode == FINDAREA_ALL ||
								SearchMode == FINDAREA_ALL_BUTNETWORK ||
								SearchMode == FINDAREA_INPATH)
								Global->CtrlObject->Plugins->SetDirectory(FindExitItem->Arc->hPlugin, L"\\", 0);
						}
						SetPluginDirectory(strFileName, FindExitItem->Arc->hPlugin, true); // ??? ,FindItem.Data ???
					}
				}
				else
				{
					string strSetName;
					size_t Length=strFileName.size();

					if (!Length)
						break;

					if (Length>1 && IsSlash(strFileName[Length-1]) && strFileName[Length-2] != L':')
						strFileName.pop_back();

					if (!os::fs::exists(strFileName) && (GetLastError() != ERROR_ACCESS_DENIED))
						break;

					const wchar_t *NamePtr = PointToName(strFileName);
					strSetName = NamePtr;

					if (Global->Opt->FindOpt.FindAlternateStreams)
					{
						size_t Pos = strSetName.find(L':');

						if (Pos != string::npos)
							strSetName.resize(Pos);
					}

					strFileName.resize(NamePtr-strFileName.data());
					Length=strFileName.size();

					if (Length>1 && IsSlash(strFileName[Length-1]) && strFileName[Length-2] != L':')
						strFileName.pop_back();

					if (strFileName.empty())
						break;

					if (FindPanel->GetType() != panel_type::FILE_PANEL && Global->CtrlObject->Cp()->GetAnotherPanel(FindPanel)->GetType() == panel_type::FILE_PANEL)
						FindPanel=Global->CtrlObject->Cp()->GetAnotherPanel(FindPanel);

					if ((FindPanel->GetType() != panel_type::FILE_PANEL) || (FindPanel->GetMode() != panel_mode::NORMAL_PANEL))
					// Сменим панель на обычную файловую...
					{
						FindPanel = Global->CtrlObject->Cp()->ChangePanel(FindPanel, panel_type::FILE_PANEL, TRUE, TRUE);
						FindPanel->SetVisible(true);
						FindPanel->Update(0);
					}

					// ! Не меняем каталог, если мы уже в нем находимся.
					// Тем самым добиваемся того, что выделение с элементов панели не сбрасывается.
					string strDirTmp = FindPanel->GetCurDir();
					Length=strDirTmp.size();

					if (Length>1 && IsSlash(strDirTmp[Length-1]) && strDirTmp[Length-2] != L':')
						strDirTmp.pop_back();

					if (StrCmpI(strFileName, strDirTmp))
						FindPanel->SetCurDir(strFileName,true);

					if (!strSetName.empty())
						FindPanel->GoToFile(strSetName);

					FindPanel->Show();
					FindPanel->Parent()->SetActivePanel(FindPanel);
				}
				break;
			}
		}
	return false;
}

void FindFiles::ProcessMessage(const AddMenuData& Data)
{
	switch(Data.m_Type)
	{
	case data:
		AddMenuRecord(m_ResultsDialogPtr, Data.m_FullName, Data.m_FindData, Data.m_Data, Data.m_FreeData, Data.m_Arc);
		m_EmptyArc = false;
		break;

	case push:
		m_LastDir.push(m_LastDirName);
		m_LastDirName.clear();
		m_EmptyArc = true;
		break;

	case pop:
		assert(!m_LastDir.empty());
		if (m_EmptyArc) m_LastDirName = m_LastDir.top();
		m_LastDir.pop();
		break;

	default:
		throw MAKE_FAR_EXCEPTION(L"Unknown message type");
	}
}


FindFiles::FindFiles():
	itd(std::make_unique<InterThreadData>()),
	FileMaskForFindFile(std::make_unique<filemasks>()),
	Filter(std::make_unique<FileFilter>(Global->CtrlObject->Cp()->ActivePanel().get(), FFT_FINDFILE)),
	m_TimeCheck(time_check::mode::immediate, GetRedrawTimeout()),
	m_MessageEvent(os::event::type::manual, os::event::state::signaled)
{
	_ALGO(CleverSysLog clv(L"FindFiles::FindFiles()"));

	static string strLastFindMask=L"*.*", strLastFindStr;

	static string strSearchFromRoot;
	strSearchFromRoot = msg(lng::MSearchFromRootFolder);

	static bool LastCmpCase = false, LastWholeWords = false, LastSearchInArchives = false, LastSearchHex = false, LastNotContaining = false;

	CmpCase=LastCmpCase;
	WholeWords=LastWholeWords;
	SearchInArchives=LastSearchInArchives;
	SearchHex=LastSearchHex;
	NotContaining = LastNotContaining;
	SearchMode = static_cast<FINDAREA>(Global->Opt->FindOpt.FileSearchMode.Get());
	UseFilter=Global->Opt->FindOpt.UseFilter.Get();
	strFindMask = strLastFindMask;
	strFindStr = strLastFindStr;

	do
	{
		FindExitItem = nullptr;
		FindFoldersChanged=false;
		SearchFromChanged=false;
		FindPositionChanged=false;
		Finalized=false;
		TB.reset();
		itd->ClearAllLists();
		const auto ActivePanel = Global->CtrlObject->Cp()->ActivePanel();
		PluginMode = ActivePanel->GetMode() == panel_mode::PLUGIN_PANEL && ActivePanel->IsVisible();
		PrepareDriveNameStr(strSearchFromRoot);
		static const wchar_t MasksHistoryName[] = L"Masks", TextHistoryName[] = L"SearchText";
		static const wchar_t HexMask[]=L"HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH HH";
		const wchar_t VSeparator[] = { BoxSymbols[BS_T_H1V1], BoxSymbols[BS_V1], BoxSymbols[BS_V1], BoxSymbols[BS_V1], BoxSymbols[BS_V1], BoxSymbols[BS_B_H1V1], 0 };
		FarDialogItem FindAskDlgData[]=
		{
			{DI_DOUBLEBOX,3,1,76,19,0,nullptr,nullptr,0,msg(lng::MFindFileTitle)},
			{DI_TEXT,5,2,0,2,0,nullptr,nullptr,0,msg(lng::MFindFileMasks)},
			{DI_EDIT,5,3,74,3,0,MasksHistoryName,nullptr,DIF_FOCUS|DIF_HISTORY|DIF_USELASTHISTORY,L""},
			{DI_TEXT,-1,4,0,4,0,nullptr,nullptr,DIF_SEPARATOR,L""},
			{DI_TEXT,5,5,0,5,0,nullptr,nullptr,0,L""},
			{DI_EDIT,5,6,74,6,0,TextHistoryName,nullptr,DIF_HISTORY,L""},
			{DI_FIXEDIT,5,6,74,6,0,nullptr,HexMask,DIF_MASKEDIT,L""},
			{DI_TEXT,5,7,0,7,0,nullptr,nullptr,0,L""},
			{DI_COMBOBOX,5,8,74,8,0,nullptr,nullptr,DIF_DROPDOWNLIST,L""},
			{DI_TEXT,-1,9,0,9,0,nullptr,nullptr,DIF_SEPARATOR,L""},
			{DI_CHECKBOX,5,10,0,10,0,nullptr,nullptr,0,msg(lng::MFindFileCase)},
			{DI_CHECKBOX,5,11,0,11,0,nullptr,nullptr,0,msg(lng::MFindFileWholeWords)},
			{DI_CHECKBOX,5,12,0,12,0,nullptr,nullptr,0,msg(lng::MSearchForHex)},
			{DI_CHECKBOX,5,13,0,13,NotContaining,nullptr,nullptr,0,msg(lng::MSearchNotContaining)},
			{DI_CHECKBOX,41,10,0,10,0,nullptr,nullptr,0,msg(lng::MFindArchives)},
			{DI_CHECKBOX,41,11,0,11,0,nullptr,nullptr,0,msg(lng::MFindFolders)},
			{DI_CHECKBOX,41,12,0,12,0,nullptr,nullptr,0,msg(lng::MFindSymLinks)},
			{DI_CHECKBOX,41,13,0,13,0,nullptr,nullptr,0,msg(lng::MFindAlternateStreams)},
			{DI_TEXT,-1,14,0,14,0,nullptr,nullptr,DIF_SEPARATOR,L""},
			{DI_VTEXT,39,9,0,9,0,nullptr,nullptr,DIF_BOXCOLOR,VSeparator},
			{DI_TEXT,5,15,0,15,0,nullptr,nullptr,0,msg(lng::MSearchWhere)},
			{DI_COMBOBOX,5,16,36,16,0,nullptr,nullptr,DIF_DROPDOWNLIST|DIF_LISTNOAMPERSAND,L""},
			{DI_CHECKBOX,41,16,0,16,(int)(UseFilter?BSTATE_CHECKED:BSTATE_UNCHECKED),nullptr,nullptr,DIF_AUTOMATION,msg(lng::MFindUseFilter)},
			{DI_TEXT,-1,17,0,17,0,nullptr,nullptr,DIF_SEPARATOR,L""},
			{DI_BUTTON,0,18,0,18,0,nullptr,nullptr,DIF_DEFAULTBUTTON|DIF_CENTERGROUP,msg(lng::MFindFileFind)},
			{DI_BUTTON,0,18,0,18,0,nullptr,nullptr,DIF_CENTERGROUP,msg(lng::MFindFileDrive)},
			{DI_BUTTON,0,18,0,18,0,nullptr,nullptr,DIF_CENTERGROUP|DIF_AUTOMATION|(UseFilter?0:DIF_DISABLE),msg(lng::MFindFileSetFilter)},
			{DI_BUTTON,0,18,0,18,0,nullptr,nullptr,DIF_CENTERGROUP,msg(lng::MFindFileAdvanced)},
			{DI_BUTTON,0,18,0,18,0,nullptr,nullptr,DIF_CENTERGROUP,msg(lng::MCancel)},
		};
		auto FindAskDlg = MakeDialogItemsEx(FindAskDlgData);

		if (strFindStr.empty())
			FindAskDlg[FAD_CHECKBOX_DIRS].Selected=Global->Opt->FindOpt.FindFolders;

		FarListItem li[]=
		{
			{0,msg(lng::MSearchAllDisks)},
			{0,msg(lng::MSearchAllButNetwork)},
			{0,msg(lng::MSearchInPATH)},
			{0,strSearchFromRoot.data()},
			{0,msg(lng::MSearchFromCurrent)},
			{0,msg(lng::MSearchInCurrent)},
			{0,msg(lng::MSearchInSelected)},
		};
		li[FADC_ALLDISKS+SearchMode].Flags|=LIF_SELECTED;
		FarList l={sizeof(FarList),std::size(li),li};
		FindAskDlg[FAD_COMBOBOX_WHERE].ListItems=&l;

		if (PluginMode)
		{
			OpenPanelInfo Info;
			// no lock - background thread hasn't been started yet
			Global->CtrlObject->Plugins->GetOpenPanelInfo(ActivePanel->GetPluginHandle(),&Info);

			if (!(Info.Flags & OPIF_REALNAMES))
				FindAskDlg[FAD_CHECKBOX_ARC].Flags |= DIF_DISABLE;

			if (SearchMode == FINDAREA_ALL || SearchMode == FINDAREA_ALL_BUTNETWORK)
			{
				li[FADC_ALLDISKS].Flags=0;
				li[FADC_ALLBUTNET].Flags=0;
				li[FADC_ROOT].Flags|=LIF_SELECTED;
			}

			li[FADC_ALLDISKS].Flags|=LIF_GRAYED;
			li[FADC_ALLBUTNET].Flags|=LIF_GRAYED;
			FindAskDlg[FAD_CHECKBOX_LINKS].Selected=0;
			FindAskDlg[FAD_CHECKBOX_LINKS].Flags|=DIF_DISABLE;
			FindAskDlg[FAD_CHECKBOX_STREAMS].Selected = 0;
			FindAskDlg[FAD_CHECKBOX_STREAMS].Flags |= DIF_DISABLE;
		}
		else
		{
			FindAskDlg[FAD_CHECKBOX_LINKS].Selected = Global->Opt->FindOpt.FindSymLinks;
			FindAskDlg[FAD_CHECKBOX_STREAMS].Selected = Global->Opt->FindOpt.FindAlternateStreams;
		}
		if (!(FindAskDlg[FAD_CHECKBOX_ARC].Flags & DIF_DISABLE))
			FindAskDlg[FAD_CHECKBOX_ARC].Selected=SearchInArchives;

		FindAskDlg[FAD_EDIT_MASK].strData = strFindMask;

		if (SearchHex)
			FindAskDlg[FAD_EDIT_HEX].strData = strFindStr;
		else
			FindAskDlg[FAD_EDIT_TEXT].strData = strFindStr;

		FindAskDlg[FAD_CHECKBOX_CASE].Selected=CmpCase;
		FindAskDlg[FAD_CHECKBOX_WHOLEWORDS].Selected=WholeWords;
		FindAskDlg[FAD_CHECKBOX_HEX].Selected=SearchHex;
		int ExitCode;
		const auto Dlg = Dialog::create(FindAskDlg, &FindFiles::MainDlgProc, this);
		Dlg->SetAutomation(FAD_CHECKBOX_FILTER,FAD_BUTTON_FILTER,DIF_DISABLE,DIF_NONE,DIF_NONE,DIF_DISABLE);
		Dlg->SetHelp(L"FindFile");
		Dlg->SetId(FindFileId);
		Dlg->SetPosition(-1,-1,80,21);
		Dlg->Process();
		ExitCode=Dlg->GetExitCode();
		//Рефреш текущему времени для фильтра сразу после выхода из диалога
		Filter->UpdateCurrentTime();

		if (ExitCode!=FAD_BUTTON_FIND)
		{
			return;
		}

		Global->Opt->FindCodePage = CodePage;
		CmpCase=FindAskDlg[FAD_CHECKBOX_CASE].Selected == BSTATE_CHECKED;
		WholeWords=FindAskDlg[FAD_CHECKBOX_WHOLEWORDS].Selected == BSTATE_CHECKED;
		SearchHex=FindAskDlg[FAD_CHECKBOX_HEX].Selected == BSTATE_CHECKED;
		SearchInArchives=FindAskDlg[FAD_CHECKBOX_ARC].Selected == BSTATE_CHECKED;
		NotContaining = FindAskDlg[FAD_CHECKBOX_NOTCONTAINING].Selected == BSTATE_CHECKED;

		if (FindFoldersChanged)
		{
			Global->Opt->FindOpt.FindFolders=(FindAskDlg[FAD_CHECKBOX_DIRS].Selected==BSTATE_CHECKED);
		}

		if (!PluginMode)
		{
			Global->Opt->FindOpt.FindSymLinks=(FindAskDlg[FAD_CHECKBOX_LINKS].Selected==BSTATE_CHECKED);
			Global->Opt->FindOpt.FindAlternateStreams = (FindAskDlg[FAD_CHECKBOX_STREAMS].Selected == BSTATE_CHECKED);
		}

		UseFilter=(FindAskDlg[FAD_CHECKBOX_FILTER].Selected==BSTATE_CHECKED);
		Global->Opt->FindOpt.UseFilter=UseFilter;
		strFindMask = !FindAskDlg[FAD_EDIT_MASK].strData.empty() ? FindAskDlg[FAD_EDIT_MASK].strData:L"*";

		if (SearchHex)
		{
			strFindStr = ExtractHexString(FindAskDlg[FAD_EDIT_HEX].strData);
		}
		else
			strFindStr = FindAskDlg[FAD_EDIT_TEXT].strData;

		if (!strFindStr.empty())
		{
			Global->StoreSearchString(strFindStr, SearchHex);
			Global->GlobalSearchCase=CmpCase;
			Global->GlobalSearchWholeWords=WholeWords;
		}

		switch (FindAskDlg[FAD_COMBOBOX_WHERE].ListPos)
		{
			case FADC_ALLDISKS:
				SearchMode=FINDAREA_ALL;
				break;
			case FADC_ALLBUTNET:
				SearchMode=FINDAREA_ALL_BUTNETWORK;
				break;
			case FADC_PATH:
				SearchMode=FINDAREA_INPATH;
				break;
			case FADC_ROOT:
				SearchMode=FINDAREA_ROOT;
				break;
			case FADC_FROMCURRENT:
				SearchMode=FINDAREA_FROM_CURRENT;
				break;
			case FADC_INCURRENT:
				SearchMode=FINDAREA_CURRENT_ONLY;
				break;
			case FADC_SELECTED:
				SearchMode=FINDAREA_SELECTED;
				break;
		}

		if (SearchFromChanged)
		{
			Global->Opt->FindOpt.FileSearchMode=SearchMode;
		}

		LastCmpCase=CmpCase;
		LastWholeWords=WholeWords;
		LastSearchHex=SearchHex;
		LastSearchInArchives=SearchInArchives;
		LastNotContaining = NotContaining;
		strLastFindMask = strFindMask;
		strLastFindStr = strFindStr;

		if (!strFindStr.empty())
			Editor::SetReplaceMode(false);
	}
	while (FindFilesProcess());

	Global->CtrlObject->Cp()->ActivePanel()->RefreshTitle();
}

FindFiles::~FindFiles() = default;


background_searcher::background_searcher(
	FindFiles* Owner,
	const string& FindString,
	FINDAREA SearchMode,
	uintptr_t CodePage,
	unsigned long long SearchInFirst,
	bool CmpCase,
	bool WholeWords,
	bool SearchInArchives,
	bool SearchHex,
	bool NotContaining,
	bool UseFilter,
	bool PluginMode):

	m_Owner(Owner),
	findString(),
	InFileSearchInited(),
	m_Autodetection(),
	strFindStr(FindString),
	SearchMode(SearchMode),
	CodePage(CodePage),
	SearchInFirst(SearchInFirst),
	CmpCase(CmpCase),
	WholeWords(WholeWords),
	SearchInArchives(SearchInArchives),
	SearchHex(SearchHex),
	NotContaining(NotContaining),
	UseFilter(UseFilter),
	m_PluginMode(PluginMode),
	PauseEvent(os::event::type::manual, os::event::state::signaled),
	StopEvent(os::event::type::manual, os::event::state::nonsignaled)
{
}
