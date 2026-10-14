// Objective: objective action classes (BIK/WAV/text/flag/technology): compare, read/save, edit dialogs and descriptions.
// Declared in the class headers; shared names in Objective_internal.h.
#include "stdafx.h"
#include "Objective_internal.h"
#include "resource.h"
#include "paths.h"
#include "ResourceStringSelectionDlg.h"
#include "BooleanFlagIsSetDialog.h"
#include "TextMessageDlg.h"
#include "MCLibResource.h"
#include "../resource.h"
#include "mc2fmt.h"
#include <cassert>


bool CObjectiveAction::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	if ((Species() == rhs.Species()) && (m_alignment == rhs.m_alignment)) {
		retval = true;
	}
	return retval;
}

bool CPlayBIK::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CPlayBIK *pCastedRhs = dynamic_cast<const CPlayBIK *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (mc2::str::iequals(m_pathname, pCastedRhs->m_pathname)) {
			retval = true;
		}
	}
	return retval;
}

bool CPlayBIK::Read( FitIniFile* missionFile )
{
	return EditorFit::readText(*missionFile, "AVIFileName", m_pathname) != EditorFit::TextStatus::Absent;
}

bool CPlayBIK::Save( FitIniFile* file )
{
	file->writeIdString("AVIFileName", m_pathname.c_str());
	return true;
}

bool CPlayBIK::EditDialog() {
	while (true) {
		CFileDialog selectAVIFileDialog(TRUE,_T("AVI"),_T("*.AVI"),
						 OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
						 _T("Movie (*.AVI)|*.AVI|"));
		selectAVIFileDialog.m_ofn.lpstrInitialDir = moviePath;
		if( selectAVIFileDialog.DoModal()==IDOK ) {
			CString pathname = selectAVIFileDialog.GetPathName();
			int CurrentDirectoryBufferLength = GetCurrentDirectory(0, 0);
			assert(1 <= CurrentDirectoryBufferLength);
			TCHAR *CurrentDirectoryBuffer = new TCHAR[CurrentDirectoryBufferLength];
			int ret = GetCurrentDirectory(CurrentDirectoryBufferLength, CurrentDirectoryBuffer);
			assert(CurrentDirectoryBufferLength - 1 == ret);
			ret = -1;
			if (pathname.GetLength() > (CurrentDirectoryBufferLength - 1)) {
				ret = _tcsnicmp(CurrentDirectoryBuffer, pathname.GetBuffer(0), CurrentDirectoryBufferLength - 1);
			}
			delete [] CurrentDirectoryBuffer; CurrentDirectoryBuffer = 0;
			if (0 == ret) {
				m_pathname = mc2::str::from_c(pathname.GetBuffer(0) + (CurrentDirectoryBufferLength - 1) + 1);
				return true;
			} else {
				AfxMessageBox(IDS_MUST_BE_IN_SUBDIRECTORY);
			}
		} else {
			return false;
		}
	}
}

std::string CPlayBIK::Description() {
	std::string retval = "PlayBIK"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_PlayBIK, retval);
	return retval;
}

std::string CPlayBIK::InstanceDescription() {
	return m_pathname;
}

bool CPlayWAV::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CPlayWAV *pCastedRhs = dynamic_cast<const CPlayWAV *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (mc2::str::iequals(m_pathname, pCastedRhs->m_pathname)) {
			retval = true;
		}
	}
	return retval;
}

bool CPlayWAV::Read( FitIniFile* missionFile )
{
	return EditorFit::readText(*missionFile, "WAVFileName", m_pathname) != EditorFit::TextStatus::Absent;
}

bool CPlayWAV::Save( FitIniFile* file )
{
	file->writeIdString("WAVFileName", m_pathname.c_str());
	return true;
}

bool CPlayWAV::EditDialog() {
	while (true) {
		CFileDialog selectWAVFileDialog(TRUE,_T("WAV"),_T("*.WAV"),
						 OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
						 _T("Wavefile (*.WAV)|*.WAV|"));
		selectWAVFileDialog.m_ofn.lpstrInitialDir = soundPath;
		if( selectWAVFileDialog.DoModal()==IDOK ) {
			CString pathname = selectWAVFileDialog.GetPathName();
			int CurrentDirectoryBufferLength = GetCurrentDirectory(0, 0);
			assert(1 <= CurrentDirectoryBufferLength);
			TCHAR *CurrentDirectoryBuffer = new TCHAR[CurrentDirectoryBufferLength];
			int ret = GetCurrentDirectory(CurrentDirectoryBufferLength, CurrentDirectoryBuffer);
			assert(CurrentDirectoryBufferLength - 1 == ret);
			ret = -1;
			if (pathname.GetLength() > (CurrentDirectoryBufferLength - 1)) {
				ret = _tcsnicmp(CurrentDirectoryBuffer, pathname.GetBuffer(0), CurrentDirectoryBufferLength - 1);
			}
			delete[] CurrentDirectoryBuffer; CurrentDirectoryBuffer = 0;
			if (0 == ret) {
				m_pathname = mc2::str::from_c(pathname.GetBuffer(0) + (CurrentDirectoryBufferLength - 1) + 1);
				return true;
			} else {
				AfxMessageBox(IDS_MUST_BE_IN_SUBDIRECTORY);
			}
		} else {
			return false;
		}
	}
}

std::string CPlayWAV::Description() {
	std::string retval = "PlayWAV"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_PlayWAV, retval);
	return retval;
}

std::string CPlayWAV::InstanceDescription() {
	return m_pathname;
}

bool CDisplayTextMessage::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CDisplayTextMessage *pCastedRhs = dynamic_cast<const CDisplayTextMessage *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (m_message == pCastedRhs->m_message) {
			retval = true;
		}
	}
	return retval;
}

bool CDisplayTextMessage::Read( FitIniFile* missionFile )
{
	return EditorFit::readText(*missionFile, "Message", m_message) != EditorFit::TextStatus::Absent;
}

bool CDisplayTextMessage::Save( FitIniFile* file )
{
	file->writeIdString("Message", m_message.c_str());
	return true;
}

bool CDisplayTextMessage::EditDialog() {
	TextMessageDlg textMessageDlg;
	textMessageDlg.m_TextMessage = m_message.c_str();
	int result = textMessageDlg.DoModal();
	if (IDOK == result) {
		m_message = mc2::str::from_c(textMessageDlg.m_TextMessage.GetBuffer(0));
		return true;
	}
	return false;
}

std::string CDisplayTextMessage::Description() {
	std::string retval = "DisplayTextMessage"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DisplayTextMessage, retval);
	return retval;
}

std::string CDisplayTextMessage::InstanceDescription() {
	return m_message;
}

bool CDisplayResourceTextMessage::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CDisplayResourceTextMessage *pCastedRhs = dynamic_cast<const CDisplayResourceTextMessage *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (m_resourceStringID == pCastedRhs->m_resourceStringID) {
			retval = true;
		}
	}
	return retval;
}

bool CDisplayResourceTextMessage::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdWholeNum(missionFile, "ResourceStringID", m_resourceStringID);
	if (NO_ERR != result) { return false; }
	return true;
}

bool CDisplayResourceTextMessage::Save( FitIniFile* file )
{
	file->writeIdULong( "ResourceStringID", m_resourceStringID);
	return true;
}

bool CDisplayResourceTextMessage::EditDialog() {
	ResourceStringSelectionDlg resourceStringSelectionDlg;
	resourceStringSelectionDlg.m_SelectedResourceStringID = m_resourceStringID;
	int result = resourceStringSelectionDlg.DoModal();
	if (IDOK == result) {
		m_resourceStringID = resourceStringSelectionDlg.m_SelectedResourceStringID;
		return true;
	}
	return false;
}

std::string CDisplayResourceTextMessage::Description() {
	std::string retval = "DisplayResourceTextMessage"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DisplayResourceTextMessage, retval);
	return retval;
}

std::string CDisplayResourceTextMessage::InstanceDescription() {
	std::string resourceText;
	LoadObjectiveString(m_resourceStringID, resourceText);
	std::string tmpEStr;
	tmpEStr = std::format("{}: {}", m_resourceStringID, resourceText);
	return tmpEStr;
}

bool CSetBooleanFlag::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CSetBooleanFlag *pCastedRhs = dynamic_cast<const CSetBooleanFlag *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if ((mc2::str::iequals(m_flagID, pCastedRhs->m_flagID)) &&
			(m_value == pCastedRhs->m_value)) {
			retval = true;
		}
	}
	return retval;
}

bool CSetBooleanFlag::Read( FitIniFile* missionFile )
{
	long result = 0;
	if (auto flagID = EditorFit::flagID(*missionFile, "FlagID", "FlagIndex")) m_flagID = std::move(*flagID);
	result = sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

bool CSetBooleanFlag::Save( FitIniFile* file )
{
	file->writeIdString("FlagID", m_flagID.c_str());
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CSetBooleanFlag::EditDialog() {
	CBooleanFlagIsSetDialog BooleanFlagIsSetDlg;
	BooleanFlagIsSetDlg.m_FlagID = m_flagID.c_str();
	BooleanFlagIsSetDlg.m_Value = 1;
	if (false == m_value) { BooleanFlagIsSetDlg.m_Value = 0; }
	if (IDOK == BooleanFlagIsSetDlg.DoModal()) {
		m_flagID = mc2::str::from_c(BooleanFlagIsSetDlg.m_FlagID.GetBuffer(0));
		m_value = true;
		if (0 == BooleanFlagIsSetDlg.m_Value) { m_value = false; }
		return true;
	} else {
		return false;
	}
}

std::string CSetBooleanFlag::Description() {
	std::string retval = "SetBooleanFlag"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_SetBooleanFlag, retval);
	return retval;
}

std::string CSetBooleanFlag::InstanceDescription() {
	std::string tmpEStr;
	tmpEStr = std::format("flag name: {}, value: {}", m_flagID, mc2::fmt::as_i32(m_value));
	return tmpEStr;
}

bool CMakeNewTechnologyAvailable::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CMakeNewTechnologyAvailable *pCastedRhs = dynamic_cast<const CMakeNewTechnologyAvailable *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (mc2::str::iequals(m_purchaseFilePathname, pCastedRhs->m_purchaseFilePathname)) {
			retval = true;
		}
	}
	return retval;
}

bool CMakeNewTechnologyAvailable::Read( FitIniFile* missionFile )
{
	return EditorFit::readText(*missionFile, "PurchaseFileName", m_purchaseFilePathname) != EditorFit::TextStatus::Absent;
}

bool CMakeNewTechnologyAvailable::Save( FitIniFile* file )
{
	file->writeIdString("PurchaseFileName", m_purchaseFilePathname.c_str());
	return true;
}

bool CMakeNewTechnologyAvailable::EditDialog() {
	while (true) {
		CFileDialog selectPurchaseFileDialog(TRUE,_T("FIT"),_T("*.FIT"),
						 OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
						 _T("Purchase File (*.FIT)|*.FIT|"));
		selectPurchaseFileDialog.m_ofn.lpstrInitialDir = missionPath;
		if( selectPurchaseFileDialog.DoModal()==IDOK ) {
			CString pathname = selectPurchaseFileDialog.GetPathName();
			int CurrentDirectoryBufferLength = GetCurrentDirectory(0, 0);
			assert(1 <= CurrentDirectoryBufferLength);
			TCHAR *CurrentDirectoryBuffer = new TCHAR[CurrentDirectoryBufferLength];
			int ret = GetCurrentDirectory(CurrentDirectoryBufferLength, CurrentDirectoryBuffer);
			assert(CurrentDirectoryBufferLength - 1 == ret);
			ret = -1;
			if (pathname.GetLength() > (CurrentDirectoryBufferLength - 1)) {
				ret = _tcsnicmp(CurrentDirectoryBuffer, pathname.GetBuffer(0), CurrentDirectoryBufferLength - 1);
			}
			delete[] CurrentDirectoryBuffer; CurrentDirectoryBuffer = 0;
			if (0 == ret) {
				m_purchaseFilePathname = mc2::str::from_c(pathname.GetBuffer(0) + (CurrentDirectoryBufferLength - 1) + 1);
				return true;
			} else {
				/* Put a dialog box saying that the wav must be in a subdirectory of the runtime 
				directory. */
			}
		} else {
			return false;
		}
	}
}

std::string CMakeNewTechnologyAvailable::Description() {
	std::string retval = "MakeNewTechnologyAvailable"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_MakeNewTechnologyAvailable, retval);
	return retval;
}

std::string CMakeNewTechnologyAvailable::InstanceDescription() {
	return m_purchaseFilePathname;
}
