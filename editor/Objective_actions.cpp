// Objective: objective action classes (BIK/WAV/text/flag/technology): compare, read/save, edit dialogs and descriptions.
// Declared in the class headers; shared names in Objective_internal.h.
#include "Objective_internal.h"


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
		if (m_pathname == pCastedRhs->m_pathname) {
			retval = true;
		}
	}
	return retval;
}

bool CPlayBIK::Read( FitIniFile* missionFile )
{
	long result = 0;
	EString tmpECStr;
	result = sReadIdString(missionFile, "AVIFileName", tmpECStr);
	if (NO_ERR != result) { return false; }
	m_pathname = tmpECStr.Data();
	return true;
}

bool CPlayBIK::Save( FitIniFile* file )
{
	file->writeIdString( "AVIFileName", m_pathname.Data());
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
				m_pathname = (pathname.GetBuffer(0) + (CurrentDirectoryBufferLength - 1) + 1);
				return true;
			} else {
				AfxMessageBox(IDS_MUST_BE_IN_SUBDIRECTORY);
			}
		} else {
			return false;
		}
	}
}

EString CPlayBIK::Description() {
	EString retval = "PlayBIK"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_PlayBIK, retval);
	return retval;
}

EString CPlayBIK::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_pathname.Data());
	return tmpEStr;
}

bool CPlayWAV::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CPlayWAV *pCastedRhs = dynamic_cast<const CPlayWAV *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (m_pathname == pCastedRhs->m_pathname) {
			retval = true;
		}
	}
	return retval;
}

bool CPlayWAV::Read( FitIniFile* missionFile )
{
	long result = 0;
	EString tmpECStr;
	result = sReadIdString(missionFile, "WAVFileName", tmpECStr);
	if (NO_ERR != result) { return false; }
	m_pathname = tmpECStr.Data();
	return true;
}

bool CPlayWAV::Save( FitIniFile* file )
{
	file->writeIdString( "WAVFileName", m_pathname.Data());
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
				m_pathname = (pathname.GetBuffer(0) + (CurrentDirectoryBufferLength - 1) + 1);
				return true;
			} else {
				AfxMessageBox(IDS_MUST_BE_IN_SUBDIRECTORY);
			}
		} else {
			return false;
		}
	}
}

EString CPlayWAV::Description() {
	EString retval = "PlayWAV"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_PlayWAV, retval);
	return retval;
}

EString CPlayWAV::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_pathname.Data());
	return tmpEStr;
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
	long result = 0;
	EString tmpECStr;
	result = sReadIdString(missionFile, "Message", tmpECStr);
	if (NO_ERR != result) { return false; }
	m_message = tmpECStr.Data();
	return true;
}

bool CDisplayTextMessage::Save( FitIniFile* file )
{
	file->writeIdString( "Message", m_message.Data());
	return true;
}

bool CDisplayTextMessage::EditDialog() {
	TextMessageDlg textMessageDlg;
	textMessageDlg.m_TextMessage = m_message.Data();
	int result = textMessageDlg.DoModal();
	if (IDOK == result) {
		m_message = textMessageDlg.m_TextMessage.GetBuffer(0);
		return true;
	}
	return false;
}

EString CDisplayTextMessage::Description() {
	EString retval = "DisplayTextMessage"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DisplayTextMessage, retval);
	return retval;
}

EString CDisplayTextMessage::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_message.Data());
	return tmpEStr;
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

EString CDisplayResourceTextMessage::Description() {
	EString retval = "DisplayResourceTextMessage"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DisplayResourceTextMessage, retval);
	return retval;
}

EString CDisplayResourceTextMessage::InstanceDescription() {
	EString resourceText;
	ESLoadString(m_resourceStringID, resourceText);
	EString tmpEStr;
	tmpEStr.Format("%d: %s", m_resourceStringID, resourceText.Data());
	return tmpEStr;
}

bool CSetBooleanFlag::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CSetBooleanFlag *pCastedRhs = dynamic_cast<const CSetBooleanFlag *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if ((m_flagID == pCastedRhs->m_flagID) &&
			(m_value == pCastedRhs->m_value)) {
			retval = true;
		}
	}
	return retval;
}

bool CSetBooleanFlag::Read( FitIniFile* missionFile )
{
	long result = 0;
	EString tmpECStr;
	result = sReadIdString(missionFile, "FlagID", tmpECStr);
	if (NO_ERR != result) {
		/*for backward compatibility*/
		int flagIndex = 0;
		result = sReadIdWholeNum(missionFile, "FlagIndex", flagIndex);
		if (NO_ERR != result) { return false; }
		m_flagID.Format("%d", flagIndex);
	} else {
		m_flagID = tmpECStr.Data();
	}
	result = sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

bool CSetBooleanFlag::Save( FitIniFile* file )
{
	file->writeIdString("FlagID", m_flagID.Data());
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CSetBooleanFlag::EditDialog() {
	CBooleanFlagIsSetDialog BooleanFlagIsSetDlg;
	BooleanFlagIsSetDlg.m_FlagID = m_flagID.Data();
	BooleanFlagIsSetDlg.m_Value = 1;
	if (false == m_value) { BooleanFlagIsSetDlg.m_Value = 0; }
	if (IDOK == BooleanFlagIsSetDlg.DoModal()) {
		m_flagID = BooleanFlagIsSetDlg.m_FlagID.GetBuffer(0);
		m_value = true;
		if (0 == BooleanFlagIsSetDlg.m_Value) { m_value = false; }
		return true;
	} else {
		return false;
	}
}

EString CSetBooleanFlag::Description() {
	EString retval = "SetBooleanFlag"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_SetBooleanFlag, retval);
	return retval;
}

EString CSetBooleanFlag::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("flag name: %s, value: %d", m_flagID.Data(), m_value);
	return tmpEStr;
}

bool CMakeNewTechnologyAvailable::operator==(const CObjectiveAction &rhs) const {
	bool retval = false;
	const CMakeNewTechnologyAvailable *pCastedRhs = dynamic_cast<const CMakeNewTechnologyAvailable *>(&rhs);
	if ((pCastedRhs) && (CObjectiveAction::operator==(rhs))) {
		if (m_purchaseFilePathname == pCastedRhs->m_purchaseFilePathname) {
			retval = true;
		}
	}
	return retval;
}

bool CMakeNewTechnologyAvailable::Read( FitIniFile* missionFile )
{
	long result = 0;
	EString tmpECStr;
	result = sReadIdString(missionFile, "PurchaseFileName", tmpECStr);
	if (NO_ERR != result) { return false; }
	m_purchaseFilePathname = tmpECStr.Data();
	return true;
}

bool CMakeNewTechnologyAvailable::Save( FitIniFile* file )
{
	file->writeIdString( "PurchaseFileName", m_purchaseFilePathname.Data());
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
				m_purchaseFilePathname = (pathname.GetBuffer(0) + (CurrentDirectoryBufferLength - 1) + 1);
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

EString CMakeNewTechnologyAvailable::Description() {
	EString retval = "MakeNewTechnologyAvailable"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_MakeNewTechnologyAvailable, retval);
	return retval;
}

EString CMakeNewTechnologyAvailable::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_purchaseFilePathname.Data());
	return tmpEStr;
}
