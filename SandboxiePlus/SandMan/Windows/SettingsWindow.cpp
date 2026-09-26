#include "stdafx.h"
#include "SettingsWindow.h"
#include "EditorSettingsWindow.h"
#include "SandMan.h"
#include "../MiscHelpers/Common/Settings.h"
#include "../MiscHelpers/Common/Common.h"
#include "../MiscHelpers/Common/OtherFunctions.h"
#include "Helpers/WinAdmin.h"
#include "../QSbieAPI/Sandboxie/SbieTemplates.h"
#include "../QSbieAPI/SbieUtils.h"
#include "OptionsWindow.h"
#include "../OnlineUpdater.h"
#include "../MiscHelpers/Archive/ArchiveFS.h"
#include <QJsonDocument>
#include "../Helpers/StorageInfo.h"
#include "../Wizards/TemplateWizard.h"
#include "../AddonManager.h"
#include <qfontdialog.h>
#include <QJsonObject>
#include "Helpers/TabOrder.h"
#include "../MiscHelpers/Common/CodeEdit.h"
#include "Helpers/IniHighlighter.h"
#include "../MiscHelpers/Common/CheckableMessageBox.h"
#include <QFileIconProvider>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>


#include <windows.h>
#include <shellapi.h>

static int CGetWindowsMonitorNumber(const QString& screenName)
{
	int markerPos = screenName.lastIndexOf("DISPLAY", -1, Qt::CaseInsensitive);
	if (markerPos < 0)
		return -1;

	QString tail = screenName.mid(markerPos + 7);
	if (tail.isEmpty())
		return -1;

	int digitCount = 0;
	while (digitCount < tail.length() && tail[digitCount].isDigit())
		digitCount++;
	if (digitCount == 0)
		return -1;

	bool ok = false;
	int number = tail.left(digitCount).toInt(&ok);
	return ok ? number : -1;
}

static int CGetDisplayNumberForScreen(QScreen* screen)
{
	if (!screen)
		return -1;

	QRect geometry = screen->geometry();
	QPoint center = geometry.center();
	POINT point = { center.x(), center.y() };
	HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
		return -1;

	MONITORINFOEXW monitorInfo;
	memset(&monitorInfo, 0, sizeof(monitorInfo));
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfoW(monitor, &monitorInfo))
		return -1;

	QString deviceName = QString::fromWCharArray(monitorInfo.szDevice);
	int displayNumber = CGetWindowsMonitorNumber(deviceName);
	if (displayNumber > 0)
		return displayNumber;

	return -1;
}

static constexpr int c_MonitorLabelBaseRole = Qt::UserRole + 100;
static constexpr int c_MonitorLabelDefaultRole = Qt::UserRole + 101;

static QString CBuildMonitorOptionLabel(const QString& baseLabel, bool isDefault, bool isFallback)
{
	QString label = baseLabel;
	if (isDefault)
		label = QObject::tr("%1 (%2)").arg(label, QObject::tr("default"));
	if (isFallback)
		label = QObject::tr("%1 (%2)").arg(label, QObject::tr("fallback"));
	return label;
}

static void CAddMonitorComboItem(QComboBox* pCombo, const QString& baseLabel, int value, bool isDefault = false)
{
	pCombo->addItem(CBuildMonitorOptionLabel(baseLabel, isDefault, false), value);
	int idx = pCombo->count() - 1;
	pCombo->setItemData(idx, baseLabel, c_MonitorLabelBaseRole);
	pCombo->setItemData(idx, isDefault, c_MonitorLabelDefaultRole);
}

static void CUpdateMonitorComboFallbackLabel(QComboBox* pCombo, int fallbackIndex, bool markFallback)
{
	for (int i = 0; i < pCombo->count(); i++) {
		QString baseLabel = pCombo->itemData(i, c_MonitorLabelBaseRole).toString();
		if (baseLabel.isEmpty())
			baseLabel = pCombo->itemText(i);
		bool isDefault = pCombo->itemData(i, c_MonitorLabelDefaultRole).toBool();
		bool isFallback = markFallback && i == fallbackIndex;
		pCombo->setItemText(i, CBuildMonitorOptionLabel(baseLabel, isDefault, isFallback));
	}
}


void FixTriStateBoxPallete(QWidget* pWidget)
{
	if (QApplication::style()->objectName() == "windows") {

		//
		// the built in "windows" theme of Qt does not properly rendered PartiallyChecked
		// checkboxes, to remedi this issue we connect to the stateChanged slot
		// and change the pattern to improve the rendering.
		//

		foreach(QCheckBox* pCheck, pWidget->findChildren<QCheckBox*>()) {
			QObject::connect(pCheck, &QCheckBox::stateChanged, [pCheck](int state) {
				if (pCheck->isTristate()) {
					QPalette palette = QApplication::palette();
					if (state == Qt::PartiallyChecked)
						palette.setColor(QPalette::Base, Qt::darkGray);
					pCheck->setPalette(palette);
				}
			});
		}
	}
}


void AddIconToLabel(QLabel* pLabel, const QPixmap& Pixmap)
{
	if (pLabel->property("hidden").toBool()) return;
	QWidget* pParent = pLabel->parentWidget();
	QWidget* pWidget = new QWidget(pParent);
	pParent->layout()->replaceWidget(pLabel, pWidget);
	QHBoxLayout* pLayout = new QHBoxLayout(pWidget);
	pLayout->setContentsMargins(0, 0, 0, 0);
	pLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	pLayout->setSpacing(1);
	QLabel* pIcon = new QLabel();
	pIcon = new QLabel();
	pIcon->setPixmap(Pixmap);
	pLayout->addWidget(pIcon);
	pLayout->addWidget(pLabel);
}


int CSettingsWindow__Chk2Int(Qt::CheckState state)
{
	switch (state) {
	case Qt::Unchecked: return 0;
	case Qt::Checked: return 1;
	default:
	case Qt::PartiallyChecked: return 2;
	}
}

Qt::CheckState CSettingsWindow__Int2Chk(int state)
{
	switch (state) {
	case 0: return Qt::Unchecked;
	case 1: return Qt::Checked;
	default:
	case 2: return Qt::PartiallyChecked;
	}
}

static bool CSettingsWindow__IsPendingHighlightEnabled(Qt::CheckState state)
{
	if (state == Qt::PartiallyChecked)
		return theConf->GetInt("Options/ViewMode", 1) != 2;
	return state == Qt::Checked;
}

quint32 g_FeatureFlags = 0;

CSettingsWindow::CSettingsWindow(QWidget* parent)
	: CConfigDialog(parent)
{
	Qt::WindowFlags flags = windowFlags();
	flags |= Qt::CustomizeWindowHint;
	//flags &= ~Qt::WindowContextHelpButtonHint;
	//flags &= ~Qt::WindowSystemMenuHint;
	//flags &= ~Qt::WindowMinMaxButtonsHint;
	flags |= Qt::WindowMinimizeButtonHint;
	//flags &= ~Qt::WindowCloseButtonHint;
	setWindowFlags(flags);

	this->setWindowFlag(Qt::WindowStaysOnTopHint, theGUI->IsAlwaysOnTop());

	ui.setupUi(this);
	this->setWindowTitle(tr("Sandboxie Plus - Global Settings"));

	if (theConf->GetBool("Options/AltRowColors", false)) {
		foreach(QTreeWidget* pTree, this->findChildren<QTreeWidget*>()) 
			pTree->setAlternatingRowColors(true);
	}

	FixTriStateBoxPallete(this);

	ui.tabs->setTabPosition(QTabWidget::West);

	ui.tabs->setCurrentIndex(0);
	ui.tabs->setTabIcon(0, CSandMan::GetIcon("Config"));
	ui.tabs->setTabIcon(1, CSandMan::GetIcon("Shell"));
	ui.tabs->setTabIcon(2, CSandMan::GetIcon("Design"));
	ui.tabs->setTabIcon(3, CSandMan::GetIcon("Plugins"));
	ui.tabs->setTabIcon(4, CSandMan::GetIcon("Support"));
	ui.tabs->setTabIcon(5, CSandMan::GetIcon("Advanced"));
	ui.tabs->setTabIcon(6, CSandMan::GetIcon("Control"));
	ui.tabs->setTabIcon(7, CSandMan::GetIcon("Compatibility"));
	ui.tabs->setTabIcon(8, CSandMan::GetIcon("Editor"));

	ui.tabsGeneral->setCurrentIndex(0);
	ui.tabsGeneral->setTabIcon(0, CSandMan::GetIcon("Presets"));
	ui.tabsGeneral->setTabIcon(1, CSandMan::GetIcon("Notification"));

	ui.tabsShell->setCurrentIndex(0);
	ui.tabsShell->setTabIcon(0, CSandMan::GetIcon("Windows"));
	ui.tabsShell->setTabIcon(1, CSandMan::GetIcon("TaskBar"));
	ui.tabsShell->setTabIcon(2, CSandMan::GetIcon("Run"));

	ui.tabsGUI->setCurrentIndex(0);
	ui.tabsGUI->setTabIcon(0, CSandMan::GetIcon("Interface"));
	ui.tabsGUI->setTabIcon(1, CSandMan::GetIcon("Monitor"));

	ui.tabsAddons->setCurrentIndex(0);
	ui.tabsAddons->setTabIcon(0, CSandMan::GetIcon("Plugin"));
	ui.tabsAddons->setTabIcon(1, CSandMan::GetIcon("Qube"));

	ui.tabsSupport->setCurrentIndex(0);
	ui.tabsSupport->setTabIcon(0, CSandMan::GetIcon("Update"));

	ui.tabsAdvanced->setCurrentIndex(0);
	ui.tabsAdvanced->setTabIcon(0, CSandMan::GetIcon("Options"));
	ui.tabsAdvanced->setTabIcon(1, CSandMan::GetIcon("EditIni"));

	ui.tabsControl->setCurrentIndex(0);
	ui.tabsControl->setTabIcon(0, CSandMan::GetIcon("Alarm"));
	ui.tabsControl->setTabIcon(1, CSandMan::GetIcon("Force"));
	ui.tabsControl->setTabIcon(2, CSandMan::GetIcon("USB"));

	ui.tabsTemplates->setCurrentIndex(0);
	ui.tabsTemplates->setTabIcon(0, CSandMan::GetIcon("Program"));
	ui.tabsTemplates->setTabIcon(1, CSandMan::GetIcon("Template"));

	int size = 16.0;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	size *= (QApplication::desktop()->logicalDpiX() / 96.0); // todo Qt6
#endif
	AddIconToLabel(ui.lblGeneral, CSandMan::GetIcon("Options").pixmap(size,size));
	AddIconToLabel(ui.lblRecovery, CSandMan::GetIcon("Recover").pixmap(size,size));
	AddIconToLabel(ui.lblNotify, CSandMan::GetIcon("Notification").pixmap(size,size));
	AddIconToLabel(ui.lblMessages, CSandMan::GetIcon("Email").pixmap(size,size));
	//AddIconToLabel(ui.lblMessages, CSandMan::GetIcon("Filter").pixmap(size,size));

	AddIconToLabel(ui.lblStartUp, CSandMan::GetIcon("Start").pixmap(size,size));
	AddIconToLabel(ui.lblRunBoxed, CSandMan::GetIcon("Run").pixmap(size,size));
	AddIconToLabel(ui.lblStartMenu, CSandMan::GetIcon("StartMenu").pixmap(size,size));
#ifdef INSIDER_BUILD
	AddIconToLabel(ui.lblDesktop, CSandMan::GetIcon("Monitor").pixmap(size,size));
#endif
	AddIconToLabel(ui.lblSysTray, CSandMan::GetIcon("Maintenance").pixmap(size,size));

	AddIconToLabel(ui.lblInterface, CSandMan::GetIcon("GUI").pixmap(size,size));

	AddIconToLabel(ui.lblDisplay, CSandMan::GetIcon("Advanced").pixmap(size,size));
	AddIconToLabel(ui.lblEditor, CSandMan::GetIcon("EditIni").pixmap(size,size));

	AddIconToLabel(ui.lblDiskImage, CSandMan::GetIcon("Disk").pixmap(size,size));

	AddIconToLabel(ui.lblBoxRoot, CSandMan::GetIcon("Sandbox").pixmap(size,size));
	AddIconToLabel(ui.lblBoxFeatures, CSandMan::GetIcon("Miscellaneous").pixmap(size,size));

	AddIconToLabel(ui.lblIni, CSandMan::GetIcon("Editor").pixmap(size,size));
	AddIconToLabel(ui.lblProtection, CSandMan::GetIcon("Lock").pixmap(size,size));

	AddIconToLabel(ui.lblUpdates, CSandMan::GetIcon("Update").pixmap(size,size));

	int iViewMode = theConf->GetInt("Options/ViewMode", 1);

	/*if (iViewMode == 0)
	{
		if (iOptionLayout == 1)
			ui.tabs->removeTab(6); // ini edit
		else 
			ui.tabs->removeTab(7); // ini edit
	}*/

	ui.tabs->setCurrentIndex(0);

	{
		ui.uiLang->addItem(tr("Auto Detection"), "");
		ui.uiLang->addItem("No Translation (English)", "native"); // do not translate

		QString langDir;
		C7zFileEngineHandler LangFS("lang", this);
		if (LangFS.Open(QApplication::applicationDirPath() + "/translations.7z"))
			langDir = LangFS.Prefix() + "/";
		else
			langDir = QApplication::applicationDirPath() + "/translations/";

		foreach(const QString & langFile, QDir(langDir).entryList(QStringList("sandman_*.qm"), QDir::Files))
		{
			QString Code = langFile.mid(8, langFile.length() - 8 - 3);
			QLocale Locale(Code);
			QString Lang = Locale.nativeLanguageName();
			ui.uiLang->addItem(Lang, Code);
		}
		ui.uiLang->addItem(tr("Arabic (Morocco) - LTR"), "ar_MA");
		ui.uiLang->setCurrentIndex(ui.uiLang->findData(theConf->GetString("Options/UiLanguage")));
	}

	ui.cmbIntegrateMenu->addItem(tr("Don't integrate links"));
	ui.cmbIntegrateMenu->addItem(tr("As sub group"));
	ui.cmbIntegrateMenu->addItem(tr("Fully integrate"));

	ui.cmbIntegrateDesk->addItem(tr("Don't integrate links"));
	ui.cmbIntegrateDesk->addItem(tr("As sub group"));
	ui.cmbIntegrateDesk->addItem(tr("Fully integrate"));

	ui.cmbSysTray->addItem(tr("Don't show any icon"));
	ui.cmbSysTray->addItem(tr("Show Plus icon"));
	ui.cmbSysTray->addItem(tr("Show Classic icon"));

	ui.cmbTrayBoxes->addItem(tr("All Boxes"));
	ui.cmbTrayBoxes->addItem(tr("Active + Pinned"));
	ui.cmbTrayBoxes->addItem(tr("Pinned Only"));
	ui.cmbTrayStatusTipModifier->addItem(tr("Ctrl"), "Ctrl");
	ui.cmbTrayStatusTipModifier->addItem(tr("Shift"), "Shift");

	ui.cmbOnClose->addItem(tr("Close to Tray"), "ToTray");
	ui.cmbOnClose->addItem(tr("Prompt before Close"), "Prompt");
	ui.cmbOnClose->addItem(tr("Close"), "Close");
	ui.cmbOnClose->addItem(tr("Hide (Run invisible in Background)"), "Hide");
	ui.cmbBoxAliasDisplayMode->addItem(tr("Sandbox name"), 1);
	ui.cmbBoxAliasDisplayMode->addItem(tr("Sandbox alias"), 0);
	ui.cmbBoxAliasDisplayMode->addItem(tr("Sandbox alias and name"), 2);
	ui.cmbBoxAliasDisplayMode->setCurrentIndex(1);

	const QString sameAsMainLabel = tr("Same as main");
	const QString keepCurrentLabel = tr("Keep current monitor");
	const QString activeMonitorLabel = tr("Active monitor");
	const QString primaryMonitorLabel = tr("Primary monitor");

	auto addWindowMonitorOptions = [&](QComboBox* pCombo, bool includeSameAsMain)
	{
		if (includeSameAsMain)
			CAddMonitorComboItem(pCombo, sameAsMainLabel, -2);
		CAddMonitorComboItem(pCombo, keepCurrentLabel, -1, true);
		CAddMonitorComboItem(pCombo, activeMonitorLabel, -3);
		CAddMonitorComboItem(pCombo, primaryMonitorLabel, 0);
	};

	addWindowMonitorOptions(ui.cmbLaunchMonitor, false);
	addWindowMonitorOptions(ui.cmbNonMainLaunchMonitor, true);
	addWindowMonitorOptions(ui.cmbRecoveryLaunchMonitor, true);
	addWindowMonitorOptions(ui.cmbNotificationLaunchMonitor, true);

	CAddMonitorComboItem(ui.cmbFallbackActiveMonitor, tr("Disabled"), -4);
	CAddMonitorComboItem(ui.cmbFallbackActiveMonitor, sameAsMainLabel, -2);
	CAddMonitorComboItem(ui.cmbFallbackActiveMonitor, activeMonitorLabel, -3, true);
	CAddMonitorComboItem(ui.cmbFallbackActiveMonitor, primaryMonitorLabel, 0);

	QList<QScreen*> screens = QGuiApplication::screens();
	QList<int> windowsDisplayIds;
	for (int i = 0; i < screens.count(); i++) {
		int displayId = CGetDisplayNumberForScreen(screens[i]);
		if (displayId > 0 && !windowsDisplayIds.contains(displayId))
			windowsDisplayIds.append(displayId);
	}
	std::sort(windowsDisplayIds.begin(), windowsDisplayIds.end());

	struct SDisplayEntry {
		int labelNumber;
		int monitorSettingData;
		QString label;
	};

	QList<SDisplayEntry> displayEntries;
	int fallbackLabelNumber = windowsDisplayIds.count() + 1;
	for (int i = 0; i < screens.count(); i++) {
		QScreen* screen = screens[i];
		int displayId = CGetDisplayNumberForScreen(screen);

		int displayLabelNumber = -1;
		if (displayId > 0) {
			int labelIndex = windowsDisplayIds.indexOf(displayId);
			if (labelIndex >= 0)
				displayLabelNumber = labelIndex + 1;
		}
		if (displayLabelNumber < 1)
			displayLabelNumber = fallbackLabelNumber++;

		QString displayName = screen->model().trimmed();
		if (displayName.isEmpty())
			displayName = screen->name();
		if (displayName.isEmpty())
			displayName = tr("Unknown Display");

		QString label = tr("Display %1: %2").arg(displayLabelNumber).arg(displayName);
		if (screen == QGuiApplication::primaryScreen())
			label += tr(" (Primary)");

		int monitorSettingData = (displayId > 0) ? displayId : (i + 1);
		displayEntries.append({ displayLabelNumber, monitorSettingData, label });
	}

	std::sort(displayEntries.begin(), displayEntries.end(), [](const SDisplayEntry& left, const SDisplayEntry& right) {
		if (left.labelNumber != right.labelNumber)
			return left.labelNumber < right.labelNumber;
		return left.label < right.label;
	});

	for (const SDisplayEntry& entry : displayEntries) {
		CAddMonitorComboItem(ui.cmbLaunchMonitor, entry.label, entry.monitorSettingData);
		CAddMonitorComboItem(ui.cmbNonMainLaunchMonitor, entry.label, entry.monitorSettingData);
		CAddMonitorComboItem(ui.cmbRecoveryLaunchMonitor, entry.label, entry.monitorSettingData);
		CAddMonitorComboItem(ui.cmbNotificationLaunchMonitor, entry.label, entry.monitorSettingData);
	}

	ui.cmbGrouping->addItem(tr("Remember previous state"), 0);
	ui.cmbGrouping->addItem(tr("Expand all groups"), 1);
	ui.cmbGrouping->addItem(tr("Collapse all groups"), 2);

	ui.cmbDPI->addItem(tr("None"), 0);
	ui.cmbDPI->addItem(tr("Native"), 1);
	ui.cmbDPI->addItem(tr("Qt"), 2);

	int FontScales[] = { 75,100,125,150,175,200,225,250,275,300,350,400, 0 };
	for (int* pFontScales = FontScales; *pFontScales != 0; pFontScales++)
		ui.cmbFontScale->addItem(tr("%1").arg(*pFontScales), *pFontScales);

	QSettings CurrentVersion("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", QSettings::NativeFormat);
	if (CurrentVersion.value("CurrentBuild").toInt() >= 22000) { // Windows 11
		QCheckBox* SecretCheckBox = new CSecretCheckBox(ui.chkShellMenu->text());
		((QGridLayout*)((QWidget*)ui.chkShellMenu->parent())->layout())->replaceWidget(ui.chkShellMenu, SecretCheckBox);
		ui.chkShellMenu->deleteLater();
		ui.chkShellMenu = SecretCheckBox;
	}

	m_HoldChange = false;
	m_SkipSaveOnToggle = false;

	DWORD logical_drives = GetLogicalDrives();
	for (CHAR search = 'D'; search <= 'Z'; search++) {
		if ((logical_drives & (1 << (search - 'A'))) == 0)
			ui.cmbRamLetter->addItem(QString("%1:\\").arg(QChar(search)));
	}

	ui.fileRoot->addItem("\\??\\%SystemDrive%\\Sandbox\\%USER%\\%SANDBOX%");
	ui.fileRoot->addItem("\\??\\%SystemDrive%\\Sandbox\\%SANDBOX%");
    ui.fileRoot->addItem("\\??\\%SystemDrive%\\Users\\%USER%\\Sandbox\\%SANDBOX%");

	ui.regRoot->addItem("\\REGISTRY\\USER\\Sandbox_%USER%_%SANDBOX%");

	ui.ipcRoot->addItem("\\Sandbox\\%USER%\\%SANDBOX%\\Session_%SESSION%");

	CPathEdit* pEditor = new CPathEdit();
	ui.txtEditor->parentWidget()->layout()->replaceWidget(ui.txtEditor, pEditor);
	ui.txtEditor->deleteLater();
	ui.txtEditor = pEditor->GetEdit();

	LoadSettings();

	ui.buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);

	connect(theGUI, SIGNAL(DrivesChanged()), this, SLOT(UpdateDrives()));

	// General Config
	connect(ui.uiLang, SIGNAL(currentIndexChanged(int)), this, SLOT(OnChangeGUI()));

	connect(ui.chkSandboxUrls, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkAutoTerminate, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkMonitorSize, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkPanic, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.keyPanic, SIGNAL(keySequenceChanged(const QKeySequence &)), this, SLOT(OnOptChanged()));
	connect(ui.chkTop, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.keyTop, SIGNAL(keySequenceChanged(const QKeySequence &)), this, SLOT(OnOptChanged()));
	connect(ui.chkPauseForce, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.keyPauseForce, SIGNAL(keySequenceChanged(const QKeySequence &)), this, SLOT(OnOptChanged()));
	connect(ui.chkSuspend, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.keySuspend, SIGNAL(keySequenceChanged(const QKeySequence&)), this, SLOT(OnOptChanged()));
	connect(ui.chkAsyncBoxOps, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));

	connect(ui.chkSilentMode, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkCopyProgress, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkNoMessages, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));

	connect(ui.btnAddMessage, SIGNAL(clicked(bool)), this, SLOT(OnAddMessage()));
	connect(ui.btnDelMessage, SIGNAL(clicked(bool)), this, SLOT(OnDelMessage()));
	connect(ui.treeMessages, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this, SLOT(OnMessageChanged()));
	m_MessagesChanged = false;

	connect(ui.chkNotifyRecovery, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkShowRecovery, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkCheckDelete, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkRecoveryTop, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));

	connect(ui.chkUseW11Style, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkRandomGuidName, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	QOperatingSystemVersion current = QOperatingSystemVersion::current();
	ui.chkUseW11Style->setEnabled(current.majorVersion() == 10 && current.microVersion() >= 22000); // Windows 10 22000+ (Windows 11)
	//

	// Shell Integration
	connect(ui.chkAutoStart, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkSvcStart, SIGNAL(stateChanged(int)), this, SLOT(OnGeneralChanged()));
	
	connect(ui.chkShellMenu, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkAlwaysDefault, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkRememberLast, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkShellMenu2, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkShellMenu3, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkShellMenu4, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));


	connect(ui.chkScanMenu, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbIntegrateMenu, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbIntegrateDesk, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));

#ifdef INSIDER_BUILD
	connect(ui.chkDeskAutoSwitch, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkDeskQuickSwitch, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
#else
	ui.lblDesktop->setVisible(false);
	ui.chkDeskAutoSwitch->setVisible(false);
	ui.chkDeskQuickSwitch->setVisible(false);
#endif
	
	connect(ui.cmbSysTray, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbTrayBoxes, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkCompactTray, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.cmbOnClose, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkBoxOpsNotify, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkTraySearch, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkMinimize, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkSingleShow, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkTrayIcons, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkTrayOverlayIcons, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkTrayUseAlias, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.spnTrayAliasChars, SIGNAL(valueChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkTrayStatusTip, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbTrayStatusTipModifier, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	auto updateTrayAliasCharsUi = [this]() {
		ui.spnTrayAliasChars->setEnabled(ui.chkTrayUseAlias->isChecked());
	};
	connect(ui.chkTrayUseAlias, &QCheckBox::stateChanged, this, [updateTrayAliasCharsUi](int) {
		updateTrayAliasCharsUi();
	});
	updateTrayAliasCharsUi();
	ui.chkTrayStatusTip->setTristate(true);
	auto updateTrayStatusTipUi = [this]() {
		QString modText = ui.cmbTrayStatusTipModifier->currentText();
		if (modText.isEmpty())
			modText = tr("Ctrl");
		ui.chkTrayStatusTip->setToolTip(tr("Tri-state behavior: unchecked = never, partially checked = only while %1 is held (default), checked = always.").arg(modText));
		ui.cmbTrayStatusTipModifier->setEnabled(ui.chkTrayStatusTip->checkState() == Qt::PartiallyChecked);
	};
	connect(ui.cmbTrayStatusTipModifier, &QComboBox::currentTextChanged, this, [updateTrayStatusTipUi](const QString&) {
		updateTrayStatusTipUi();
	});
	connect(ui.chkTrayStatusTip, &QCheckBox::stateChanged, this, [updateTrayStatusTipUi](int) {
		updateTrayStatusTipUi();
	});
	updateTrayStatusTipUi();
	//

	// Interface Config
	connect(ui.cmbDPI, SIGNAL(currentIndexChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkDarkTheme, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkFusionTheme, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkAltRows, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkBackground, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkLargeIcons, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkNoIcons, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkOptTree, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkNewLayout, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkColorIcons, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkOverlayIcons, SIGNAL(stateChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.chkHideCore, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.chkHighlightPendingChanges, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbGrouping, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbLaunchMonitor, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbNonMainLaunchMonitor, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbRecoveryLaunchMonitor, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbNotificationLaunchMonitor, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));
	connect(ui.cmbFallbackActiveMonitor, SIGNAL(currentIndexChanged(int)), this, SLOT(OnOptChanged()));


	connect(ui.cmbFontScale, SIGNAL(currentIndexChanged(int)), this, SLOT(OnChangeGUI()));
	connect(ui.cmbFontScale, SIGNAL(currentTextChanged(const QString&)), this, SLOT(OnChangeGUI()));
	connect(ui.chkHide, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));

	// UI Font
	ui.btnSelectUiFont->setIcon(CSandMan::GetIcon("Font"));
	ui.btnSelectUiFont->setToolTip(tr("Select font"));
	ui.btnResetUiFont->setIcon(CSandMan::GetIcon("ResetFont"));
	ui.btnResetUiFont->setToolTip(tr("Reset font"));

	connect(ui.btnSelectUiFont, SIGNAL(clicked(bool)), this, SLOT(OnSelectUiFont()));
	connect(ui.btnResetUiFont, SIGNAL(clicked(bool)), this, SLOT(OnResetUiFont()));
	ui.lblUiFont->setText(QApplication::font().family());

	connect(ui.txtEditor, SIGNAL(textChanged(const QString&)), this, SLOT(OnOptChanged()));
	m_bRebuildUI = false;
	//

	// Run tab
	connect(ui.btnAddCmd, SIGNAL(clicked(bool)), this, SLOT(OnAddCommand()));
	QMenu* pRunBtnMenu = new QMenu(ui.btnAddCmd);
	pRunBtnMenu->addAction(tr("Browse for Program"), this, SLOT(OnBrowsePath()));
	ui.btnAddCmd->setPopupMode(QToolButton::MenuButtonPopup);
	ui.btnAddCmd->setMenu(pRunBtnMenu);
	connect(ui.btnCmdUp, SIGNAL(clicked(bool)), this, SLOT(OnCommandUp()));
	connect(ui.btnCmdDown, SIGNAL(clicked(bool)), this, SLOT(OnCommandDown()));
	connect(ui.btnDelCmd, SIGNAL(clicked(bool)), this, SLOT(OnDelCommand()));
	connect(ui.treeRun, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this, SLOT(OnRunChanged()));
	m_RunChanged = false;
	//

	// Addons
	QObject::connect(theGUI->GetAddonManager(), &CAddonManager::DataUpdated, this, [=]() {
		ui.lblUpdateAddons->setVisible(false);
		OnLoadAddon();
	});

	connect(ui.btnInstallAddon, SIGNAL(clicked(bool)), this, SLOT(OnInstallAddon()));
	connect(ui.btnRemoveAddon, SIGNAL(clicked(bool)), this, SLOT(OnRemoveAddon()));

	connect(ui.lblUpdateAddons, &QLabel::linkActivated, this, [=]() {
		theGUI->GetAddonManager()->UpdateAddons();
	});

	connect(ui.chkRamDisk, SIGNAL(stateChanged(int)), this, SLOT(OnRamDiskChange()));
	connect(ui.lblImDisk, SIGNAL(linkActivated(const QString&)), theGUI, SLOT(OpenUrl(const QString&)));
	QObject::connect(theGUI->GetAddonManager(), &CAddonManager::AddonInstalled, this, [=] {
		if (!theGUI->GetAddonManager()->GetAddon("ImDisk", CAddonManager::eInstalled).isNull()) {
			ui.lblImDisk->setVisible(false);
			ui.chkRamDisk->setEnabled(true);
			OnRamDiskChange();
		}
	});
	connect(ui.txtRamLimit, SIGNAL(textChanged(const QString&)), this, SLOT(OnRamDiskChange()));
	connect(ui.chkRamLetter, SIGNAL(stateChanged(int)), this, SLOT(OnRamDiskChange()));
	connect(ui.cmbRamLetter, SIGNAL(currentIndexChanged(int)), this, SLOT(OnGeneralChanged()));
	//

	// Advanced Config
	connect(ui.cmbDefault, SIGNAL(currentIndexChanged(int)), this, SLOT(OnGeneralChanged()));
	connect(ui.chkAutoRoot, SIGNAL(stateChanged(int)), this, SLOT(OnRootChanged())); // not sbie ini
	connect(ui.fileRoot, SIGNAL(currentTextChanged(const QString&)), this, SLOT(OnGeneralChanged()));
	connect(ui.chkLockBox, SIGNAL(stateChanged(int)), this, SLOT(OnGeneralChanged()));
	connect(ui.regRoot, SIGNAL(currentTextChanged(const QString&)), this, SLOT(OnGeneralChanged()));
	connect(ui.ipcRoot, SIGNAL(currentTextChanged(const QString&)), this, SLOT(OnGeneralChanged()));
	
	connect(ui.chkWFP, SIGNAL(stateChanged(int)), this, SLOT(OnFeaturesChanged()));
	connect(ui.chkObjCb, SIGNAL(stateChanged(int)), this, SLOT(OnFeaturesChanged()));
	if (CurrentVersion.value("CurrentBuild").toInt() < 14393) // Windows 10 RS1 and later
		ui.chkWin32k->setEnabled(false);
	//connect(ui.chkWin32k, SIGNAL(stateChanged(int)), this, SLOT(OnFeaturesChanged()));
	m_FeaturesChanged = false;
	connect(ui.chkWin32k, SIGNAL(stateChanged(int)), this, SLOT(OnGeneralChanged()));
	connect(ui.chkSbieLogon, SIGNAL(stateChanged(int)), this, SLOT(OnGeneralChanged()));
	connect(ui.chkSbieAll, SIGNAL(stateChanged(int)), this, SLOT(OnGeneralChanged()));
	connect(ui.chkSbieUAC, SIGNAL(stateChanged(int)), this, SLOT(OnGeneralChanged()));
	connect(ui.cmbBoxAliasDisplayMode, SIGNAL(currentIndexChanged(int)), this, SLOT(OnGeneralChanged()));
	m_GeneralChanged = false;

	connect(ui.chkWatchConfig, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged())); // not sbie ini

	connect(ui.btnAddBox, SIGNAL(clicked(bool)), this, SLOT(OnImportBox()));
	connect(ui.btnMkBox, SIGNAL(clicked(bool)), this, SLOT(OnMakeBox()));
	connect(ui.btnAddRoot, SIGNAL(clicked(bool)), this, SLOT(OnAddRoot()));
	connect(ui.btnRemBox, SIGNAL(clicked(bool)), this, SLOT(OnRemoveBox()));
	m_ImportChanged = false;

	connect(ui.chkSkipUAC, SIGNAL(stateChanged(int)), this, SLOT(OnSkipUAC()));
	ui.chkSkipUAC->setEnabled(IsElevated());
	m_SkipUACChanged = false;

	connect(ui.chkAdminOnly, SIGNAL(stateChanged(int)), this, SLOT(OnProtectionChange()));
	connect(ui.chkPassRequired, SIGNAL(stateChanged(int)), this, SLOT(OnProtectionChange()));
	connect(ui.btnSetPassword, SIGNAL(clicked(bool)), this, SLOT(OnSetPassword()));
	connect(ui.chkAdminOnlyFP, SIGNAL(stateChanged(int)), this, SLOT(OnProtectionChange()));
	connect(ui.chkClearPass, SIGNAL(stateChanged(int)), this, SLOT(OnProtectionChange()));
	
	m_ProtectionChanged = false;
	//
	
	// Program Control
	connect(ui.chkStartBlock, SIGNAL(stateChanged(int)), this, SLOT(OnWarnChanged()));

	connect(ui.chkStartBlockMsg, SIGNAL(stateChanged(int)), this, SLOT(OnWarnChanged()));
	connect(ui.chkNotForcedMsg, SIGNAL(stateChanged(int)), this, SLOT(OnWarnChanged()));
	connect(ui.chkForcedMsg, SIGNAL(stateChanged(int)), this, SLOT(OnWarnChanged()));
	connect(ui.btnAddWarnProg, SIGNAL(clicked(bool)), this, SLOT(OnAddWarnProg()));
	connect(ui.btnAddWarnFolder, SIGNAL(clicked(bool)), this, SLOT(OnAddWarnFolder()));
	connect(ui.btnDelWarnProg, SIGNAL(clicked(bool)), this, SLOT(OnDelWarnProg()));

	connect(ui.btnBrowse, SIGNAL(clicked(bool)), this, SLOT(OnBrowse()));
	m_WarnProgsChanged = false;
	//

	connect(ui.chkSandboxMoTW, SIGNAL(stateChanged(int)), this, SLOT(OnMoTWChange()));
	connect(ui.cmbMoTWSandbox, SIGNAL(currentIndexChanged(int)), this, SLOT(OnMoTWChange()));
	connect(ui.chkForceBoxDocs, SIGNAL(stateChanged(int)), this, SLOT(OnOptChanged()));

	// USB
	connect(ui.chkSandboxUsb, SIGNAL(stateChanged(int)), this, SLOT(OnVolumeChanged()));
	connect(ui.cmbUsbSandbox, SIGNAL(currentIndexChanged(int)), this, SLOT(OnVolumeChanged()));
	connect(ui.treeVolumes, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this, SLOT(OnVolumeChanged()));
	m_VolumeChanged = false;
	// 

	// Templates
	connect(ui.btnAddCompat, SIGNAL(clicked(bool)), this, SLOT(OnAddCompat()));
	connect(ui.btnDelCompat, SIGNAL(clicked(bool)), this, SLOT(OnDelCompat()));
	m_CompatLoaded = 0;
	m_CompatChanged = false;
	ui.chkNoCompat->setChecked(!theConf->GetBool("Options/AutoRunSoftCompat", true));

	connect(ui.treeCompat, SIGNAL(itemClicked(QTreeWidgetItem*, int)), this, SLOT(OnTemplateClicked(QTreeWidgetItem*, int)));
	connect(ui.treeCompat, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)), this, SLOT(OnTemplateDoubleClicked(QTreeWidgetItem*, int)));

	connect(ui.txtTemplates, SIGNAL(textChanged(const QString&)), this, SLOT(OnFilterTemplates()));
	//connect(ui.treeTemplates, SIGNAL(itemClicked(QTreeWidgetItem*, int)), this, SLOT(OnTemplateClicked(QTreeWidgetItem*, int)));
	connect(ui.treeTemplates, SIGNAL(itemChanged(QTreeWidgetItem*, int)), this, SLOT(OnTemplateClicked(QTreeWidgetItem*, int)));
	connect(ui.treeTemplates, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)), this, SLOT(OnTemplateDoubleClicked(QTreeWidgetItem*, int)));
	connect(ui.btnAddTemplate, SIGNAL(clicked(bool)), this, SLOT(OnAddTemplates()));
	QMenu* pTmplBtnMenu = new QMenu(ui.btnAddTemplate);
	for(int i = 1; i < CTemplateWizard::TmplMax; i++)
		pTmplBtnMenu->addAction(tr("Add %1 Template").arg(CTemplateWizard::GetTemplateLabel((CTemplateWizard::ETemplateType)i)), this, SLOT(OnTemplateWizard()))->setData(i);
	ui.btnAddTemplate->setPopupMode(QToolButton::MenuButtonPopup);
	ui.btnAddTemplate->setMenu(pTmplBtnMenu);
	connect(ui.btnOpenTemplate, SIGNAL(clicked(bool)), this, SLOT(OnOpenTemplate()));
	connect(ui.btnDelTemplate, SIGNAL(clicked(bool)), this, SLOT(OnDelTemplates()));

	//

	connect(ui.chkUpdateIssues, SIGNAL(toggled(bool)), this, SLOT(OnOptChanged()));
	connect(ui.chkUpdateAddons, SIGNAL(toggled(bool)), this, SLOT(OnOptChanged()));

	//

	connect(ui.tabs, SIGNAL(currentChanged(int)), this, SLOT(OnTab()));

	// Ini Edit

	ui.btnSelectIniFont->setIcon(CSandMan::GetIcon("Font"));
	ui.btnSelectIniFont->setToolTip(tr("Select font"));
	ui.btnResetIniFont->setIcon(CSandMan::GetIcon("ResetFont"));
	ui.btnResetIniFont->setToolTip(tr("Reset font"));

	// Initialize validation flag from config, fallback to checkbox if not set
	bool defaultValidation = theConf->GetBool("Options/ValidateIniKeys", ui.chkValidateIniKeys->isChecked());
	ui.chkValidateIniKeys->setChecked(defaultValidation);
	m_IniValidationEnabled = defaultValidation;

	int defaultTooltip = theConf->GetInt("Options/EnableIniTooltips", static_cast<int>(CIniHighlighter::GetTooltipMode()));
	ui.chkEnableTooltips->setTristate(true); // Enable tri-state
	ui.chkEnableTooltips->setCheckState(static_cast<Qt::CheckState>(defaultTooltip));
	CIniHighlighter::SetTooltipMode(defaultTooltip); // Initialize the mode

	LoadCompletionConsent();
	int defaultAutoCompletion = theConf->GetInt("Options/EnableAutoCompletion", static_cast<int>(CCodeEdit::GetAutoCompletionMode()));
	if (m_AutoCompletionConsent) { // Consented
		ui.chkEnableAutoCompletion->setTristate(true); // Enable tri-state
		ui.chkEnableAutoCompletion->setCheckState(static_cast<Qt::CheckState>(defaultAutoCompletion));
		CCodeEdit::SetAutoCompletionMode(defaultAutoCompletion); // Initialize the mode
	}
	else {
		CCodeEdit::SetAutoCompletionMode(Qt::Unchecked);
		ui.chkEnableAutoCompletion->setCheckState(Qt::Unchecked);
	}

	// Create initial highlighter and editor
	m_pIniHighlighter = new CIniHighlighter(theGUI->m_DarkTheme, nullptr, m_IniValidationEnabled);
	m_pCodeEdit = new CCodeEdit(m_pIniHighlighter);
	m_pCodeEdit->installEventFilter(this);
	ui.txtIniSection->parentWidget()->layout()->replaceWidget(ui.txtIniSection, m_pCodeEdit);
	delete ui.txtIniSection;
	ui.txtIniSection = nullptr;
	connect(m_pCodeEdit, SIGNAL(textChanged()), this, SLOT(OnIniChanged()));

	// Set fuzzy prefix length bounds from settings data
	CCodeEdit::SetMaxFuzzyPrefixLength(CIniHighlighter::getMaxSettingNameLengthOrDefault());
	CCodeEdit::SetMinFuzzyPrefixLength(CIniHighlighter::getMinSettingNameLengthOrDefault());
	// Pass fuzzy matching toggle from config (no UI checkbox required)
	m_pCodeEdit->SetFuzzyMatchingEnabled(theConf->GetBool("Options/EnableFuzzyMatching", false));

	// Show tooltips when navigating with keyboard
	{
		int iniMode = theConf->GetInt("Options/EnableIniTooltips", static_cast<int>(CIniHighlighter::GetTooltipMode()));
		int popupMode = theConf->GetInt("Options/EnablePopupTooltips", iniMode);
		CCodeEdit::SetPopupTooltipsEnabled(popupMode);
	}

	m_pCodeEdit->SetCompletionFilterCallback([](const QString& keyName, const QString& inputKey) -> bool {
		return CIniHighlighter::IsKeyHiddenFromPopup(keyName)
			|| CIniHighlighter::ShouldHideCompletionCandidate(inputKey, keyName, 'p');
	});
	m_pCodeEdit->SetCompletionInsertionCallback([](const QString& candidateKey) -> QString {
		return CIniHighlighter::GetCompletionInsertionText(candidateKey);
	});
	m_pCodeEdit->SetCompletionMatchTextCallback([](const QString& candidateKey) -> QString {
		return CIniHighlighter::GetCompletionMatchText(candidateKey);
	});
	m_pCodeEdit->SetCompletionScoringCallback([](const QStringList& candidates, const QString& inputKey) -> QHash<QString, int> {
		return CIniHighlighter::GetCompletionScores(candidates, inputKey);
	});
	m_pCodeEdit->SetCaseCorrectionCallback([](const QString& wrongKey) -> QString {
		return CIniHighlighter::FindCaseCorrectedKey(wrongKey);
		});
	m_pCodeEdit->SetCaseCorrectionFilterCallback([](const QString& keyName, const QString& inputKey) -> bool {
		return CIniHighlighter::IsKeyHiddenFromContext(keyName, 'c')
			|| CIniHighlighter::ShouldHideCompletionCandidate(inputKey, keyName, 'c');
		});
	m_pCodeEdit->SetPopupTooltipCallback([](const QString& keyName) -> QString {
		return CIniHighlighter::GetSettingTooltipForPopup(keyName, QString(), 'g');
		});
	connect(m_pCodeEdit, SIGNAL(autoCompletionModeChanged(int)), this, SLOT(OnAutoCompletionModeChanged(int)));
	ApplyAutoCompletionMode(static_cast<int>(CCodeEdit::GetAutoCompletionMode()));

	ApplyIniEditFont();

	connect(ui.btnSelectIniFont, SIGNAL(clicked(bool)), this, SLOT(OnSelectIniEditFont()));
	connect(ui.btnResetIniFont, SIGNAL(clicked(bool)), this, SLOT(OnResetIniEditFont()));
	connect(ui.btnEditIni, SIGNAL(clicked(bool)), this, SLOT(OnEditIni()));
	connect(ui.btnEditorSettings, SIGNAL(clicked(bool)), this, SLOT(OnEditorSettings()));
	connect(ui.chkValidateIniKeys, SIGNAL(stateChanged(int)), this, SLOT(OnIniValidationToggled(int)));
	connect(ui.chkEnableTooltips, SIGNAL(stateChanged(int)), this, SLOT(OnTooltipToggled(int)));
	connect(ui.chkEnableAutoCompletion, SIGNAL(stateChanged(int)), this, SLOT(OnAutoCompletionToggled(int)));
	connect(ui.btnSaveIni, SIGNAL(clicked(bool)), this, SLOT(OnSaveIni()));
	connect(ui.btnCancelEdit, SIGNAL(clicked(bool)), this, SLOT(OnCancelEdit()));
	//connect(ui.txtIniSection, SIGNAL(textChanged()), this, SLOT(OnIniChanged()));
	//

	connect(ui.buttonBox->button(QDialogButtonBox::Ok), SIGNAL(clicked(bool)), this, SLOT(ok()));
	connect(ui.buttonBox->button(QDialogButtonBox::Apply), SIGNAL(clicked(bool)), this, SLOT(apply()));
	connect(ui.buttonBox, SIGNAL(rejected()), this, SLOT(reject()));

	this->installEventFilter(this); // prevent enter from closing the dialog

	restoreGeometry(theConf->GetBlob("SettingsWindow/Window_Geometry"));

	foreach(QTreeWidget * pTree, this->findChildren<QTreeWidget*>()) {
		QByteArray Columns = theConf->GetBlob("SettingsWindow/" + pTree->objectName() + "_Columns");
		if (!Columns.isEmpty()) 
			pTree->header()->restoreState(Columns);
		}

	int iOptionTree = theConf->GetInt("Options/OptionTree", 2);
	if (iOptionTree == 2)
		iOptionTree = iViewMode == 2 ? 1 : 0;

	if (iOptionTree) 
		OnSetTree();
	else {
		QWidget* pSearch = AddConfigSearch(ui.tabs);
		ui.horizontalLayout->insertWidget(0, pSearch);
		QTimer::singleShot(0, [this]() {
			m_pSearch->setMaximumWidth(m_pTabs->tabBar()->width());
		});

		QAction* pSetTree = new QAction();
		connect(pSetTree, SIGNAL(triggered()), this, SLOT(OnSetTree()));
		pSetTree->setShortcut(QKeySequence("Ctrl+Alt+T"));
		pSetTree->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		this->addAction(pSetTree);
	}
	m_pSearch->setPlaceholderText(tr("Search for settings"));

	SetTabOrder(this);
}

void CSettingsWindow::ApplyIniEditFont()
{
	QFont font; // defaults to application font
	auto fontName = theConf->GetString("UIConfig/IniFont", "").trimmed();	
	if (!fontName.isEmpty()) {
		font.fromString(fontName); // ignore fromString() fail
	    //ui.txtIniSection->setFont(font);
		m_pCodeEdit->SetFont(font);
	}
	ui.lblIniEditFont->setText(tr("%0, %1 pt").arg(font.family()).arg(font.pointSizeF())); // tr: example: "Calibri, 9.5 pt"
}

void CSettingsWindow::UpdateAutoCompletion()
{
	if (CCodeEdit::GetAutoCompletionMode() == CCodeEdit::AutoCompletionMode::Disabled || !m_pCodeEdit || !m_pCodeEdit->GetCompleter())
		return;

	// Get completion candidates from the highlighter
	QStringList candidates = CIniHighlighter::GetCompletionCandidates();

	// Update the completion model
	m_pCodeEdit->UpdateCompletionModel(candidates);
}

void CSettingsWindow::ApplyAutoCompletionMode(int state)
{
	if (!m_pCodeEdit)
		return;

	if (!m_pCodeEdit->GetCompleter()) {
		QCompleter* completer = new QCompleter(m_pCodeEdit);
		completer->setModelSorting(QCompleter::CaseInsensitivelySortedModel);
		completer->setFilterMode(Qt::MatchContains);
		m_pCodeEdit->SetCompleter(completer);
	}

	if (state == Qt::Unchecked) {
		m_pCodeEdit->HideCompletionPopup();
		return;
	}

	UpdateAutoCompletion();
}

void CSettingsWindow::OnAutoCompletionModeChanged(int state)
{
	ApplyAutoCompletionMode(state);
}

void CSettingsWindow::OnSelectIniEditFont()
{
	bool ok;
	//auto newFont = QFontDialog::getFont(&ok, ui.txtIniSection->font(), this);
	auto newFont = QFontDialog::getFont(&ok, m_pCodeEdit->GetFont(), this);
	if (!ok) return;
	theConf->SetValue("UIConfig/IniFont", newFont.toString());
	ApplyIniEditFont();
}

void CSettingsWindow::OnResetIniEditFont()
{
	theConf->DelValue("UIConfig/IniFont");
	ApplyIniEditFont();
}


void CSettingsWindow::OnSelectUiFont()
{
	bool ok;
	auto newFont = QFontDialog::getFont(&ok, QApplication::font(), this);
	if (!ok) return;
	ui.lblUiFont->setText(newFont.family());
	OnChangeGUI();
}

void CSettingsWindow::OnResetUiFont()
{
	QFont defaultFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
	ui.lblUiFont->setText(defaultFont.family());
	OnChangeGUI();
}

void CSettingsWindow::OnSetTree()
{
	if (!ui.tabs) return;
	QWidget* pAltView = ConvertToTree(ui.tabs);
	ui.verticalLayout->replaceWidget(ui.tabs, pAltView);
	ui.tabs->deleteLater();
	ui.tabs = NULL;
}

CSettingsWindow::~CSettingsWindow()
{
	theConf->SetBlob("SettingsWindow/Window_Geometry",saveGeometry());

	foreach(QTreeWidget * pTree, this->findChildren<QTreeWidget*>()) 
		theConf->SetBlob("SettingsWindow/" + pTree->objectName() + "_Columns", pTree->header()->saveState());
}

void CSettingsWindow::showTab(const QString& Name, bool bExclusive, bool bExec)
{
	QWidget* pWidget = this->findChild<QWidget*>("tab" + Name);

	if (ui.tabs) {
		
		for (int i = 0; i < ui.tabs->count(); i++) {
			QGridLayout* pGrid = qobject_cast<QGridLayout*>(ui.tabs->widget(i)->layout());
			QTabWidget* pSubTabs = pGrid ? qobject_cast<QTabWidget*>(pGrid->itemAt(0)->widget()) : NULL;
			if(ui.tabs->widget(i) == pWidget)
				ui.tabs->setCurrentIndex(i);
			else if(pSubTabs) {
				for (int j = 0; j < pSubTabs->count(); j++) {
					if (pSubTabs->widget(j) == pWidget) {
						ui.tabs->setCurrentIndex(i);
						pSubTabs->setCurrentIndex(j);
					}
				}
			}
		}
		
	}
	else
		m_pStack->setCurrentWidget(pWidget);

	if(pWidget == ui.tabCompat)
		m_CompatLoaded = 2;

	if (bExclusive) {
		if (ui.tabs)
			ui.tabs->tabBar()->setVisible(false);
		else {
			m_pTree->setVisible(false);
			m_pSearch->setVisible(false);
		}
	}

	if (bExec)
		theGUI->SafeExec(this);
	else
		CSandMan::SafeShow(this);
}

void CSettingsWindow::closeEvent(QCloseEvent *e)
{
	emit Closed();
	this->deleteLater();
}

bool CSettingsWindow::eventFilter(QObject *source, QEvent *event)
{
	if (event->type() == QEvent::KeyPress && ((QKeyEvent*)event)->key() == Qt::Key_Escape 
		&& ((QKeyEvent*)event)->modifiers() == Qt::NoModifier
		&& source == m_pCodeEdit)
	{
		return true; // cancel event
	}

	if (event->type() == QEvent::KeyPress && (((QKeyEvent*)event)->key() == Qt::Key_Enter || ((QKeyEvent*)event)->key() == Qt::Key_Return)
		&& (((QKeyEvent*)event)->modifiers() == Qt::NoModifier || ((QKeyEvent*)event)->modifiers() == Qt::KeypadModifier))
	{
		return true; // cancel event
	}

	// Tooltip handling
	if (source == m_pCodeEdit && event->type() == QEvent::ToolTip) {
		// Check if tooltips are completely disabled
		if (CIniHighlighter::GetTooltipMode() == CIniHighlighter::TooltipMode::Disabled)
			return false;

		QHelpEvent* helpEvent = static_cast<QHelpEvent*>(event);

		// Find the text edit widget inside CCodeEdit
		QTextEdit* pTextEdit = m_pCodeEdit->GetTextEdit();
		if (pTextEdit) {
			// Convert mouse position to text cursor position
			QPoint pos = pTextEdit->viewport()->mapFrom(m_pCodeEdit, helpEvent->pos());
			QTextCursor cursor = pTextEdit->cursorForPosition(pos);

			// Get the current line to check if it's a comment
			QTextBlock block = cursor.block();
			QString currentLine = block.text();

			// Don't show tooltips for comment lines
			if (CIniHighlighter::IsCommentLine(currentLine))
				return false;

			// Custom word selection that includes dots and underscores
			int initialPos = cursor.position() - block.position();
			int startPos = initialPos;
			int endPos = initialPos;

			// Move to start of the word
			while (startPos > 0) {
				QChar c = currentLine[startPos - 1];
				if (c.isLetterOrNumber() || c == '_' || c == '.')
					startPos--;
				else
					break;
			}

			// Move to end of the word
			while (endPos < currentLine.length()) {
				QChar c = currentLine[endPos];
				if (c.isLetterOrNumber() || c == '_' || c == '.')
					endPos++;
				else
					break;
			}

			// Show tooltip if it's a valid setting
			if (CIniHighlighter::IsValidTooltipContext(currentLine.left(endPos))) {
				// Only try to show tooltips if settings are loaded
				if (CIniHighlighter::IsSettingsLoaded()) {
					QString settingName = currentLine.mid(startPos, endPos - startPos);
					if (settingName.endsWith('='))
						settingName.chop(1);
					const int equalsIndex = currentLine.indexOf('=');
					const QString settingValue = equalsIndex >= 0 ? currentLine.mid(equalsIndex + 1).trimmed() : QString();
					QString tooltipText = CIniHighlighter::GetSettingTooltip(settingName, settingValue, 'g');
					if (!tooltipText.isEmpty()) {
						QToolTip::showText(helpEvent->globalPos(), tooltipText, pTextEdit);
						return true;
					}
				}
			}
			QToolTip::hideText();
		}
	}

	return QDialog::eventFilter(source, event);
}

void CSettingsWindow::OnAddMessage()
{
	QString Value = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter message"), QLineEdit::Normal);
	if (Value.isEmpty())
		return;

	StrPair IdText = Split2(Value, ",");

	AddMessageItem(IdText.first, IdText.second);

	OnMessageChanged();
}

void CSettingsWindow::AddMessageItem(const QString& ID, const QString& Text)
{
	QTreeWidgetItem* pItem = new QTreeWidgetItem();
	pItem->setText(0, ID);
	pItem->setText(1, Text);
	pItem->setFlags(pItem->flags() | Qt::ItemIsEditable);
	ui.treeMessages->addTopLevelItem(pItem);
}

void CSettingsWindow::OnDelMessage()
{
	QTreeWidgetItem* pItem = ui.treeMessages->currentItem();
	if (!pItem)
		return;

	delete pItem;
	OnMessageChanged();
}

void CSettingsWindow::OnBrowsePath()
{
	QString Value = QFileDialog::getOpenFileName(this, tr("Select Program"), "", tr("Executables (*.exe *.cmd)")).replace("/", "\\");
	if (Value.isEmpty())
		return;

	QString Name = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter a menu title"), QLineEdit::Normal);
	if (Name.isEmpty())
		return;

	QVariantMap Entry;
	Entry["Name"] = Name;
	Entry["Command"] = "\"" + Value + "\"";
	AddRunItem(ui.treeRun, Entry);
}

void CSettingsWindow::OnAddCommand()
{
	QString Value = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter a command"), QLineEdit::Normal);
	if (Value.isEmpty())
		return;

	QString Name = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter a menu title"), QLineEdit::Normal);
	if (Name.isEmpty())
		return;

	QVariantMap Entry;
	Entry["Name"] = Name;
	Entry["Command"] = Value;
	AddRunItem(ui.treeRun, Entry);

	OnRunChanged();
}

void CSettingsWindow::OnCommandUp()
{
	int index = ui.treeRun->indexOfTopLevelItem(ui.treeRun->currentItem());
	if (index > 0) {
		QTreeWidgetItem* pItem = ui.treeRun->takeTopLevelItem(index);
		ui.treeRun->insertTopLevelItem(index - 1, pItem);
		ui.treeRun->setCurrentItem(pItem);
		OnRunChanged();
	}
}

void CSettingsWindow::OnCommandDown()
{
	int index = ui.treeRun->indexOfTopLevelItem(ui.treeRun->currentItem());
	if (index < ui.treeRun->topLevelItemCount()-1) {
		QTreeWidgetItem* pItem = ui.treeRun->takeTopLevelItem(index);
		ui.treeRun->insertTopLevelItem(index + 1, pItem);
		ui.treeRun->setCurrentItem(pItem);
		OnRunChanged();
	}
}

void CSettingsWindow::OnDelCommand()
{
	QTreeWidgetItem* pItem = ui.treeRun->currentItem();
	if (!pItem)
		return;

	delete pItem;
	OnRunChanged();
}

Qt::CheckState CSettingsWindow::IsContextMenu()
{
	//QSettings Package("HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\PackagedCom\\Package", QSettings::NativeFormat);
	QSettings Package("HKEY_CURRENT_USER\\Software\\Classes\\PackagedCom\\Package", QSettings::NativeFormat);
	foreach(const QString & Key, Package.childGroups()) {
		if (Key.indexOf("SandboxieShell") == 0)
			return Qt::Checked;
	}

	QString cmd = CSbieUtils::GetContextMenuStartCmd();
	if (cmd.contains("SandMan.exe", Qt::CaseInsensitive)) 
		return Qt::Checked; // set up and sandman
	if (!cmd.isEmpty()) // ... probably sbiectrl.exe
		return Qt::PartiallyChecked; 
	return Qt::Unchecked; // not set up
}

void CSettingsWindow::AddContextMenu(bool bAlwaysClassic)
{
	QSettings CurrentVersion("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", QSettings::NativeFormat);
	if (CurrentVersion.value("CurrentBuild").toInt() >= 22000 && !bAlwaysClassic) // Windows 11
	{
		QSettings MyReg("HKEY_CURRENT_USER\\SOFTWARE\\Xanasoft\\Sandboxie-Plus\\SbieShellExt\\Lang", QSettings::NativeFormat);
		MyReg.setValue("Open Sandboxed", CSettingsWindow::tr("Run &Sandboxed"));
		MyReg.setValue("Explore Sandboxed", CSettingsWindow::tr("Run &Sandboxed"));
		
		QDir::setCurrent(QCoreApplication::applicationDirPath());
		QProcess Proc;
		Proc.execute("rundll32.exe", QStringList() << "SbieShellExt.dll,RegisterPackage");
		Proc.waitForFinished();
		return;
	}

	CSbieUtils::AddContextMenu(QApplication::applicationDirPath().replace("/", "\\") + "\\SandMan.exe",
		CSettingsWindow::tr("Run &Sandboxed")/*, //CSettingsWindow::tr("Explore &Sandboxed"),
			QApplication::applicationDirPath().replace("/", "\\") + "\\Start.exe"*/);
}

void CSettingsWindow::RemoveContextMenu()
{
	QSettings CurrentVersion("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", QSettings::NativeFormat);
	if (CurrentVersion.value("CurrentBuild").toInt() >= 22000) // Windows 11
	{
		QDir::setCurrent(QCoreApplication::applicationDirPath());
		QProcess Proc;
		Proc.execute("rundll32.exe", QStringList() << "SbieShellExt.dll,RemovePackage");
		Proc.waitForFinished();
	}

	CSbieUtils::RemoveContextMenu();
}

bool CSettingsWindow::AddBrowserIcon()
{
	QString Path = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).replace("/", "\\");
	Path += "\\" + CSettingsWindow::tr("Sandboxed Web Browser") + ".lnk";
	QString StartExe = theAPI->GetSbiePath() + "\\SandMan.exe";
	QString BoxName = theAPI->GetGlobalSettings()->GetText("DefaultBox", "DefaultBox");
	return CSbieUtils::CreateShortcut(StartExe, Path, "", BoxName, "default_browser");
}

void CSettingsWindow::LoadSettings()
{
	ui.uiLang->setCurrentIndex(ui.uiLang->findData(theConf->GetString("Options/UiLanguage")));

	ui.chkAutoStart->setChecked(IsAutorunEnabled());
	if (theAPI->IsConnected()) {
		if (theAPI->GetUserSettings()->GetBool("SbieCtrl_EnableAutoStart", true)) {
			if (theAPI->GetUserSettings()->GetText("SbieCtrl_AutoStartAgent", "").left(11) != "SandMan.exe")
				ui.chkSvcStart->setCheckState(Qt::PartiallyChecked);
			else
				ui.chkSvcStart->setChecked(true);
		}
		else
			ui.chkSvcStart->setChecked(false);
	}
	else {
		ui.chkSvcStart->setEnabled(false);
	}

	ui.chkShellMenu->setCheckState(IsContextMenu());
	ui.chkShellMenu2->setChecked(CSbieUtils::HasContextMenu2());
	ui.chkShellMenu3->setChecked(CSbieUtils::HasContextMenu3());
	ui.chkShellMenu4->setChecked(CSbieUtils::HasContextMenu4());
	ui.chkAlwaysDefault->setChecked(theConf->GetBool("Options/RunInDefaultBox", false));
	ui.chkRememberLast->setChecked(theConf->GetBool("Options/RememberLastBox", false));

	ui.cmbDPI->setCurrentIndex(theConf->GetInt("Options/DPIScaling", 1));

	ui.chkDarkTheme->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/UseDarkTheme", 2)));
	ui.chkFusionTheme->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/UseFusionTheme", 2)));
	ui.chkAltRows->setChecked(theConf->GetBool("Options/AltRowColors", false));
	ui.chkBackground->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/UseBackground", 2)));
	ui.chkLargeIcons->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/LargeIcons", 2)));
	ui.chkNoIcons->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/NoIcons", 2)));
	ui.chkOptTree->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/OptionTree", 2)));
	ui.chkNewLayout->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/NewConfigLayout", 2)));
	ui.chkColorIcons->setChecked(theConf->GetBool("Options/ColorBoxIcons", false));
	ui.chkOverlayIcons->setChecked(theConf->GetBool("Options/UseOverlayIcons", true));
	ui.chkHideCore->setChecked(theConf->GetBool("Options/HideSbieProcesses", false));
	ui.chkHighlightPendingChanges->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/HighlightPendingChanges", 2)));
	m_PendingChanges.SetEnabled(CSettingsWindow__IsPendingHighlightEnabled(ui.chkHighlightPendingChanges->checkState()), m_pTree);
	ui.cmbGrouping->setCurrentIndex(theConf->GetInt("Options/BoxGroupHandling", 0));
	


	//ui.cmbFontScale->setCurrentIndex(ui.cmbFontScale->findData(theConf->GetInt("Options/FontScaling", 100)));
	ui.cmbFontScale->setCurrentText(QString::number(theConf->GetInt("Options/FontScaling", 100)));
	ui.chkHide->setChecked(theConf->GetBool("Options/CoverWindows", false));


	ui.txtEditor->setText(theConf->GetString("Options/Editor", "notepad.exe"));

	ui.chkSilentMode->setChecked(theConf->GetBool("Options/CheckSilentMode", true));
	ui.chkCopyProgress->setChecked(theConf->GetBool("Options/ShowMigrationProgress", true));
	ui.chkNoMessages->setChecked(!theConf->GetBool("Options/ShowNotifications", true));

	ui.chkSandboxUrls->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/OpenUrlsSandboxed", 2)));
	ui.chkAutoTerminate->setChecked(theConf->GetBool("Options/TerminateWhenExit"));

	ui.chkShowRecovery->setChecked(theConf->GetBool("Options/ShowRecovery", false));
	ui.chkCheckDelete->setChecked(theConf->GetBool("Options/CleanUpOnStart", false));
	ui.chkNotifyRecovery->setChecked(!theConf->GetBool("Options/InstantRecovery", true));
	ui.chkRecoveryTop->setChecked(theConf->GetBool("Options/RecoveryOnTop", true));
	ui.chkAsyncBoxOps->setChecked(theConf->GetBool("Options/UseAsyncBoxOps", false));

	ui.chkPanic->setChecked(theConf->GetBool("Options/EnablePanicKey", false));
	ui.keyPanic->setKeySequence(QKeySequence(theConf->GetString("Options/PanicKeySequence", "Shift+Pause")));

	ui.chkTop->setChecked(theConf->GetBool("Options/EnableTopMostKey", false));
	ui.keyTop->setKeySequence(QKeySequence(theConf->GetString("Options/TopMostKeySequence", "Alt+Pause")));

	ui.chkPauseForce->setChecked(theConf->GetBool("Options/EnablePauseForceKey", false));
	ui.keyPauseForce->setKeySequence(QKeySequence(theConf->GetString("Options/PauseForceKeySequence", "Ctrl+Alt+F")));

	ui.chkSuspend->setChecked(theConf->GetBool("Options/EnableSuspendKey", false));
	ui.keySuspend->setKeySequence(QKeySequence(theConf->GetString("Options/SuspendKeySequence", "Shift+Alt+Pause")));

	ui.chkMonitorSize->setChecked(theConf->GetBool("Options/WatchBoxSize", false));

	ui.chkWatchConfig->setChecked(theConf->GetBool("Options/WatchIni", true));
	ui.chkSkipUAC->setChecked(SkipUacRun(true));

	ui.chkScanMenu->setChecked(theConf->GetBool("Options/ScanStartMenu", true));
	ui.cmbIntegrateMenu->setCurrentIndex(theConf->GetInt("Options/IntegrateStartMenu", 0));
	ui.cmbIntegrateDesk->setCurrentIndex(theConf->GetInt("Options/IntegrateDesktop", 0));

#ifdef INSIDER_BUILD
	ui.chkDeskAutoSwitch->setChecked(theConf->GetBool("Options/AutoDesktopSwitch", true));
	ui.chkDeskQuickSwitch->setChecked(theConf->GetBool("Options/QuickDesktopSwitch", true));
#endif

	ui.cmbSysTray->setCurrentIndex(theConf->GetInt("Options/SysTrayIcon", 1));
	ui.cmbTrayBoxes->setCurrentIndex(theConf->GetInt("Options/SysTrayFilter", 0));
	ui.chkCompactTray->setChecked(theConf->GetBool("Options/CompactTray", false));
	ui.chkTraySearch->setChecked(theConf->GetBool("Options/TraySearch", true));
	ui.chkTrayIcons->setChecked(theConf->GetBool("Options/TrayIcons", true));
	ui.chkTrayOverlayIcons->setChecked(theConf->GetBool("Options/TrayOverlayIcons", true));
	ui.chkTrayUseAlias->setChecked(theConf->GetBool("Options/TrayUseAlias", true));
	int iTrayAliasChars = theConf->GetInt("Options/TrayAliasMaxChars", 64);
	if (iTrayAliasChars < 32 || iTrayAliasChars > 256)
		iTrayAliasChars = 64;
	ui.spnTrayAliasChars->setValue(iTrayAliasChars);
	ui.spnTrayAliasChars->setEnabled(ui.chkTrayUseAlias->isChecked());
	// 0=never, 1=Ctrl key (default, partial-checked), 2=always
	int iTrayStatusTip = theConf->GetInt("Options/TrayStatusTip", 1);
	if (iTrayStatusTip < 0 || iTrayStatusTip > 2)
		iTrayStatusTip = 1;
	ui.chkTrayStatusTip->setCheckState(Qt::CheckState(iTrayStatusTip));
	QString trayStatusTipModifier = theConf->GetString("Options/TrayStatusTipModifier", "Ctrl");
	int trayStatusTipModifierIndex = ui.cmbTrayStatusTipModifier->findData(trayStatusTipModifier);
	if (trayStatusTipModifierIndex < 0)
		trayStatusTipModifierIndex = ui.cmbTrayStatusTipModifier->findData("Ctrl");
	ui.cmbTrayStatusTipModifier->setCurrentIndex(trayStatusTipModifierIndex);
	ui.cmbTrayStatusTipModifier->setEnabled(ui.chkTrayStatusTip->checkState() == Qt::PartiallyChecked);
	ui.chkBoxOpsNotify->setChecked(theConf->GetBool("Options/AutoBoxOpsNotify", false));
	ui.cmbOnClose->setCurrentIndex(ui.cmbOnClose->findData(theConf->GetString("Options/OnClose", "ToTray")));
	ui.chkMinimize->setChecked(theConf->GetBool("Options/MinimizeToTray", false));
	m_WindowMonitorRawSettings.clear();
	m_WindowMonitorResolvedFallback.clear();

	int mainMonitorSetting = theConf->GetInt("Options/WindowTargetMonitor", -1);
	int nonMainMonitorSetting = theConf->GetInt("Options/NonMainWindowTargetMonitor", -1);
	int recoveryMonitorSetting = theConf->GetInt("Options/RecoveryWindowTargetMonitor", nonMainMonitorSetting);
	int notificationMonitorSetting = theConf->GetInt("Options/NotificationWindowTargetMonitor", nonMainMonitorSetting);
	int fallbackSetting = theConf->GetInt("Options/WindowMonitorFallback", -3);
	if (fallbackSetting != -4 && fallbackSetting != -2 && fallbackSetting != -3 && fallbackSetting != 0)
		fallbackSetting = -3;
	int mainFallbackSetting = (fallbackSetting == 0 || fallbackSetting == -3) ? fallbackSetting : -1;
	int nonMainFallbackSetting = fallbackSetting == -4 ? -1 : fallbackSetting;

	QList<QScreen*> screens = QGuiApplication::screens();
	auto loadMonitorSetting = [&](QComboBox* pCombo, int rawSetting, int fallbackValue, const QString& settingName)
	{
		int monitorIndex = pCombo->findData(rawSetting);
		if (monitorIndex < 0 && rawSetting > 0 && rawSetting <= screens.count()) {
			int migratedDisplayNumber = CGetDisplayNumberForScreen(screens[rawSetting - 1]);
			if (migratedDisplayNumber > 0)
				monitorIndex = pCombo->findData(migratedDisplayNumber);
		}

		bool unresolvedSetting = monitorIndex < 0;
		if (unresolvedSetting)
			monitorIndex = pCombo->findData(fallbackValue);
		if (monitorIndex < 0)
			monitorIndex = 0;

		CUpdateMonitorComboFallbackLabel(pCombo, monitorIndex, unresolvedSetting);
		pCombo->setCurrentIndex(monitorIndex);

		m_WindowMonitorRawSettings.insert(settingName, rawSetting);
		if (unresolvedSetting)
			m_WindowMonitorResolvedFallback.insert(settingName, pCombo->itemData(monitorIndex).toInt());
		else
			m_WindowMonitorResolvedFallback.remove(settingName);
	};

	loadMonitorSetting(ui.cmbLaunchMonitor, mainMonitorSetting, mainFallbackSetting, "Options/WindowTargetMonitor");
	loadMonitorSetting(ui.cmbNonMainLaunchMonitor, nonMainMonitorSetting, nonMainFallbackSetting, "Options/NonMainWindowTargetMonitor");
	loadMonitorSetting(ui.cmbRecoveryLaunchMonitor, recoveryMonitorSetting, nonMainFallbackSetting, "Options/RecoveryWindowTargetMonitor");
	loadMonitorSetting(ui.cmbNotificationLaunchMonitor, notificationMonitorSetting, nonMainFallbackSetting, "Options/NotificationWindowTargetMonitor");

	int fallbackIndex = ui.cmbFallbackActiveMonitor->findData(fallbackSetting);
	if (fallbackIndex < 0)
		fallbackIndex = ui.cmbFallbackActiveMonitor->findData(-3);
	if (fallbackIndex < 0)
		fallbackIndex = 0;
	ui.cmbFallbackActiveMonitor->setCurrentIndex(fallbackIndex);
	ui.chkSingleShow->setChecked(theConf->GetBool("Options/TraySingleClick", false));

	ui.chkUseW11Style->setChecked(theConf->GetBool("Options/UseW11Style", false));
	ui.chkRandomGuidName->setChecked(theConf->GetBool("Options/UseRandomBoxName", false));

	OnLoadAddon();

	bool bImDiskReady = theGUI->IsImDiskReady();
	ui.lblImDisk->setVisible(!bImDiskReady);

	if (theAPI->IsConnected())
	{
		ui.treeMessages->clear();
		foreach(const QString & Value, theAPI->GetUserSettings()->GetTextList("SbieCtrl_HideMessage", false))
		{
			StrPair NameIcon = Split2(Value, ",");
			AddMessageItem(NameIcon.first, NameIcon.second);
		}
		m_MessagesChanged = false;

		ui.treeRun->clear();
		foreach(const QString& Value, theAPI->GetGlobalSettings()->GetTextList("RunCommand", false))
			AddRunItem(ui.treeRun, GetRunEntry(Value));
		m_RunChanged = false;
		
		ui.cmbDefault->clear();
		foreach(const CSandBoxPtr & pBox, theAPI->GetAllBoxes())
			ui.cmbDefault->addItem(pBox.objectCast<CSandBoxPlus>()->GetDisplayName(CSandBoxPlus::eDisplayCompact), pBox->GetName());
		int pos = ui.cmbDefault->findData(theAPI->GetGlobalSettings()->GetText("DefaultBox", "DefaultBox"));
		if(pos == -1)
			pos = ui.cmbDefault->findData("DefaultBox");
		ui.cmbDefault->setCurrentIndex(pos);

		QString FileRootPath_Default = "\\??\\%SystemDrive%\\Sandbox\\%USER%\\%SANDBOX%";
		QString KeyRootPath_Default  = "\\REGISTRY\\USER\\Sandbox_%USER%_%SANDBOX%";
		QString IpcRootPath_Default  = "\\Sandbox\\%USER%\\%SANDBOX%\\Session_%SESSION%";

		ui.fileRoot->setCurrentText(theAPI->GetGlobalSettings()->GetText("FileRootPath", FileRootPath_Default));
		ui.chkLockBox->setChecked(theAPI->GetGlobalSettings()->GetBool("LockBoxToUser", false)); 
		//ui.chkSeparateUserFolders->setChecked(theAPI->GetGlobalSettings()->GetBool("SeparateUserFolders", true));
		ui.regRoot->setCurrentText(theAPI->GetGlobalSettings()->GetText("KeyRootPath", KeyRootPath_Default));
		ui.ipcRoot->setCurrentText(theAPI->GetGlobalSettings()->GetText("IpcRootPath", IpcRootPath_Default));

		ui.chkRamDisk->setEnabled(bImDiskReady);
		quint32 uDiskLimit = theAPI->GetGlobalSettings()->GetNum64("RamDiskSizeKb");
		ui.chkRamDisk->setChecked(uDiskLimit > 0);
		if(uDiskLimit > 0) ui.txtRamLimit->setText(QString::number(uDiskLimit));
		QString RamLetter = theAPI->GetGlobalSettings()->GetText("RamDiskLetter");
		ui.chkRamLetter->setChecked(!RamLetter.isEmpty());
		ui.cmbRamLetter->setCurrentIndex(ui.cmbRamLetter->findText(RamLetter));
		m_HoldChange = true;
		OnRamDiskChange();
		m_HoldChange = false;

		ui.chkWFP->setChecked(theAPI->GetGlobalSettings()->GetBool("NetworkEnableWFP", false));
		ui.chkObjCb->setChecked(theAPI->GetGlobalSettings()->GetBool("EnableObjectFiltering", true));
		ui.chkWin32k->setChecked(theAPI->GetGlobalSettings()->GetBool("EnableWin32kHooks", true));
		ui.chkSbieLogon->setChecked(theAPI->GetGlobalSettings()->GetBool("SandboxieLogon", false));
		ui.chkSbieAll->setChecked(theAPI->GetGlobalSettings()->GetBool("SandboxieAllGroup", true));
		ui.chkSbieUAC->setChecked(theAPI->GetGlobalSettings()->GetBool("UseSandboxieUAC", true));
		int iBoxAliasDisplayMode = theAPI->GetGlobalSettings()->GetNum("BoxAliasDisplayMode", 2);
		if (iBoxAliasDisplayMode < 0 || iBoxAliasDisplayMode > 2)
			iBoxAliasDisplayMode = 2;
		ui.cmbBoxAliasDisplayMode->setCurrentIndex(ui.cmbBoxAliasDisplayMode->findData(iBoxAliasDisplayMode));

		ui.treeImport->clear();
		foreach(const QString& Value, theAPI->GetGlobalSettings()->GetTextList("ImportBox", false))
			ui.treeImport->addTopLevelItem(new QTreeWidgetItem(QStringList() << Value));
		m_ImportChanged = false;

		ui.chkAdminOnly->setChecked(theAPI->GetGlobalSettings()->GetBool("EditAdminOnly", false));
		ui.chkAdminOnly->setEnabled(IsAdminUser());
		ui.chkPassRequired->setChecked(!theAPI->GetGlobalSettings()->GetText("EditPassword", "").isEmpty());
		ui.chkAdminOnlyFP->setChecked(theAPI->GetGlobalSettings()->GetBool("ForceDisableAdminOnly", false));
		ui.chkClearPass->setChecked(theAPI->GetGlobalSettings()->GetBool("ForgetPassword", false));
		m_HoldChange = true;
		OnProtectionChange();
		m_HoldChange = false;

		m_GeneralChanged = false;

		ui.chkStartBlock->setChecked(theAPI->GetGlobalSettings()->GetBool("StartRunAlertDenied", false));
		ui.chkStartBlockMsg->setChecked(theAPI->GetGlobalSettings()->GetBool("AlertStartRunAccessDenied", true));
		ui.chkNotForcedMsg->setChecked(theAPI->GetGlobalSettings()->GetBool("NotifyForceProcessDisabled", false));
		ui.chkForcedMsg->setChecked(theAPI->GetGlobalSettings()->GetBool("NotifyForceProcessEnabled", false));

		ui.treeWarnProgs->clear();

		foreach(const QString& Value, theAPI->GetGlobalSettings()->GetTextList("AlertProcess", false))
			AddWarnEntry(Value, 1);
		
		foreach(const QString& Value, theAPI->GetGlobalSettings()->GetTextList("AlertFolder", false))
			AddWarnEntry(Value, 2);

		m_WarnProgsChanged = false;

		QString MoTWBox = theAPI->GetGlobalSettings()->GetText("MarkOfTheWebBox", "Web_Box");
		ui.chkSandboxMoTW->setChecked(theAPI->GetGlobalSettings()->GetBool("ForceMarkOfTheWeb", false));
		ui.chkForceBoxDocs->setChecked(theAPI->GetGlobalSettings()->GetBool("ForceBoxDocs", false));
		QString USBBox = theAPI->GetGlobalSettings()->GetText("UsbSandbox", "USB_Box");
		ui.chkSandboxUsb->setChecked(theAPI->GetGlobalSettings()->GetBool("ForceUsbDrives", false));

		ui.cmbMoTWSandbox->clear();
		ui.cmbUsbSandbox->clear();

		QFileIconProvider IconProvider;
		bool ColorIcons = theConf->GetBool("Options/ColorBoxIcons", false);

		int CurUsbBox = 0;
		int CurMoTWBox = 0;
		foreach(const CSandBoxPtr& pBox, theAPI->GetAllBoxes()) 
		{
			if (USBBox == pBox->GetName())
				CurUsbBox = ui.cmbUsbSandbox->count();

			if (MoTWBox == pBox->GetName())
				CurMoTWBox = ui.cmbMoTWSandbox->count();

			auto pBoxEx = pBox.objectCast<CSandBoxPlus>();

			QIcon Icon;
			QString Action = pBox->GetText("DblClickAction");
			if (!Action.isEmpty() && Action.left(1) != "!")
				Icon = IconProvider.icon(QFileInfo(pBoxEx->GetCommandFile(Action)));
			else if(ColorIcons)
				Icon = theGUI->GetColorIcon(pBoxEx->GetColor(), pBox->GetActiveProcessCount());
			else
				Icon = theGUI->GetBoxIcon(pBoxEx->GetType(), pBox->GetActiveProcessCount() != 0);
			ui.cmbMoTWSandbox->addItem(Icon, pBoxEx->GetDisplayName(CSandBoxPlus::eDisplayCompact), pBox->GetName());
			ui.cmbUsbSandbox->addItem(Icon, pBoxEx->GetDisplayName(CSandBoxPlus::eDisplayCompact), pBox->GetName());
		}
		ui.cmbMoTWSandbox->setCurrentIndex(CurMoTWBox);
		ui.cmbUsbSandbox->setCurrentIndex(CurUsbBox);
		
		ui.cmbMoTWSandbox->setEnabled(ui.chkSandboxMoTW->isChecked());
		ui.cmbUsbSandbox->setEnabled(ui.chkSandboxUsb->isChecked());
		ui.treeVolumes->setEnabled(ui.chkSandboxUsb->isChecked());

		UpdateDrives();

		m_VolumeChanged = false;
	}
	
	if(!theAPI->IsConnected() || (theAPI->GetGlobalSettings()->GetBool("EditAdminOnly", false) && !IsAdminUser()))
	{
		ui.cmbDefault->setEnabled(false);
		ui.fileRoot->setEnabled(false);
		//ui.chkSeparateUserFolders->setEnabled(false);
		ui.chkAutoRoot->setEnabled(false);
		ui.chkLockBox->setEnabled(false);
		ui.chkWFP->setEnabled(false);
		ui.chkObjCb->setEnabled(false);
		ui.chkWin32k->setEnabled(false);
		ui.chkSbieLogon->setEnabled(false);
		ui.chkSbieAll->setEnabled(false);
		ui.chkSbieUAC->setEnabled(false);
		ui.cmbBoxAliasDisplayMode->setEnabled(false);
		ui.regRoot->setEnabled(false);
		ui.ipcRoot->setEnabled(false);
		ui.chkRamDisk->setEnabled(false);
		ui.txtRamLimit->setEnabled(false);
		ui.lblRamLimit->setEnabled(false);
		ui.treeImport->setEnabled(false);
		ui.btnAddBox->setEnabled(false);
		ui.btnMkBox->setEnabled(false);
		ui.btnAddRoot->setEnabled(false);
		ui.btnRemBox->setEnabled(false);
		ui.chkAdminOnly->setEnabled(false);
		ui.chkPassRequired->setEnabled(false);
		ui.chkAdminOnlyFP->setEnabled(false);
		ui.chkClearPass->setEnabled(false);
		ui.btnSetPassword->setEnabled(false);
		ui.treeWarnProgs->setEnabled(false);
		ui.btnAddWarnProg->setEnabled(false);
		ui.btnDelWarnProg->setEnabled(false);
		ui.chkSandboxMoTW->setEnabled(false);
		ui.chkForceBoxDocs->setEnabled(false);
		ui.cmbMoTWSandbox->setEnabled(false);
		ui.chkSandboxUsb->setEnabled(false);
		ui.cmbUsbSandbox->setEnabled(false);
		ui.treeVolumes->setEnabled(false);
		ui.treeCompat->setEnabled(false);
		ui.btnAddCompat->setEnabled(false);
		ui.btnDelCompat->setEnabled(false);
		ui.treeTemplates->setEnabled(false);
		ui.btnAddTemplate->setEnabled(false);
		ui.btnDelTemplate->setEnabled(false);
		ui.btnEditIni->setEnabled(false);
	}


	if (theGUI->IsFullyPortable()) {
		ui.chkAutoRoot->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/PortableRootDir", 2)));
		m_HoldChange = true;
		OnRootChanged();
		m_HoldChange = false;
	}
	else
		ui.chkAutoRoot->setVisible(false);

	//ui.chkUpdateTemplates->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/CheckForTemplates", 2)));
	ui.chkUpdateAddons->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/CheckForAddons", 2)));
	ui.chkUpdateIssues->setCheckState(CSettingsWindow__Int2Chk(theConf->GetInt("Options/CheckForIssues", 2)));

	m_PendingChanges.CaptureItemBaselines(m_pTree);
	m_PendingChanges.CaptureCheckboxBaselines();
	m_PendingChanges.CaptureRadioButtonBaselines();
	m_PendingChanges.CaptureValueBaselines();
}

void CSettingsWindow::OnRamDiskChange()
{
	if (ui.chkRamDisk->isChecked() && ui.txtRamLimit->text().isEmpty())
		ui.txtRamLimit->setText(QString::number(2 * 1024 * 1024));

	bool bEnabled = ui.chkRamDisk->isChecked() && ui.chkRamDisk->isEnabled();
	ui.lblRamDisk->setEnabled(bEnabled);
	ui.txtRamLimit->setEnabled(bEnabled);
	ui.lblRamLimit->setEnabled(bEnabled);
	ui.lblRamLimit->setText(tr("kilobytes (%1)").arg(FormatSize(ui.txtRamLimit->text().toULongLong() * 1024)));

	ui.chkRamLetter->setEnabled(bEnabled);
	ui.cmbRamLetter->setEnabled(bEnabled && ui.chkRamLetter->isChecked());
	ui.lblRamLetter->setEnabled(bEnabled && ui.chkRamLetter->isChecked());

	OnGeneralChanged();
}

void CSettingsWindow::OnMoTWChange()
{
	ui.cmbMoTWSandbox->setEnabled(ui.chkSandboxMoTW->isChecked());
	OnOptChanged();
}

void CSettingsWindow::OnVolumeChanged()
{
	ui.cmbUsbSandbox->setEnabled(ui.chkSandboxUsb->isChecked());
	ui.treeVolumes->setEnabled(ui.chkSandboxUsb->isChecked());

	m_VolumeChanged = true;
	OnOptChanged();
}

void CSettingsWindow::UpdateDrives()
{
	if (!theAPI->IsConnected())
		return;

	ui.treeVolumes->clear();

	QStringList DisabledForceVolume = theAPI->GetGlobalSettings()->GetTextList("DisabledForceVolume", false);

	auto volumes = ListAllVolumes();
	auto drives = ListAllDrives();
	for (auto I = volumes.begin(); I != volumes.end(); ++I) {

		QStringList Devices;
		bool bOnUSB = false;
		for (auto J = I->disks.begin(); J != I->disks.end(); ++J) {
			SDriveInfo& info = drives[J->deviceName];
			if (info.Enum == L"USBSTOR")
				bOnUSB = true;
			Devices.append(QString::fromStdWString(info.Name));
		}

		if (bOnUSB) {
			QTreeWidgetItem* pItem = new QTreeWidgetItem();
			ui.treeVolumes->addTopLevelItem(pItem);

			QString Info = Devices.join("|");

			QStringList Mounts;
			for (auto J = I->mountPoints.begin(); J != I->mountPoints.end(); ++J)
				Mounts.append(QString::fromStdWString(*J));
			Info += " (" + Mounts.join(", ") + ")";

			std::wstring label;
			quint32 sn = CSbieAPI::GetVolumeSN(I->deviceName.c_str(), &label);
			QString SN = QString("%1-%2").arg((ushort)HIWORD(sn), 4, 16, QChar('0')).arg((ushort)LOWORD(sn), 4, 16, QChar('0')).toUpper();

			if (!label.empty())
				Info += " [" + QString::fromStdWString(label) + "]";

			pItem->setText(0, SN);
			if(DisabledForceVolume.removeAll(SN))
				pItem->setCheckState(0, Qt::Unchecked);
			else
				pItem->setCheckState(0, Qt::Checked);
			pItem->setText(1, Info);
		}
	}

	foreach(const QString & SN, DisabledForceVolume) {

		QTreeWidgetItem* pItem = new QTreeWidgetItem();
		ui.treeVolumes->addTopLevelItem(pItem);

		pItem->setText(0, SN);
		pItem->setCheckState(0, Qt::Unchecked);
		pItem->setText(1, tr("Volume not attached"));
	}
}

void CSettingsWindow::WriteAdvancedCheck(QCheckBox* pCheck, const QString& Name, const QString& OnValue, const QString& OffValue)
{
	//if (pCheck->checkState() == Qt::PartiallyChecked)
	//	return;

	if (!pCheck->isEnabled())
		return;

	SB_STATUS Status;
	if (pCheck->checkState() == Qt::Checked)
	{
		if(!OnValue.isEmpty())
			Status = theAPI->GetGlobalSettings()->SetText(Name, OnValue);
		else
			Status = theAPI->GetGlobalSettings()->DelValue(Name);
	}
	else if (pCheck->checkState() == Qt::Unchecked)
	{
		if (!OffValue.isEmpty())
			Status = theAPI->GetGlobalSettings()->SetText(Name, OffValue);
		else
			Status = theAPI->GetGlobalSettings()->DelValue(Name);
	}

	if (!Status)
		throw Status;
}

void CSettingsWindow::WriteText(const QString& Name, const QString& Value)
{
	SB_STATUS Status;
	if(Value.isEmpty())
		Status = theAPI->GetGlobalSettings()->DelValue(Name);
	else
		Status = theAPI->GetGlobalSettings()->SetText(Name, Value);
	if (!Status)
		throw Status;
}

void CSettingsWindow::WriteTextList(const QString& Setting, const QStringList& List)
{
	SB_STATUS Status = theAPI->GetGlobalSettings()->UpdateTextList(Setting, List, false);
	if (!Status)
		throw Status;
}

void CSettingsWindow::SaveSettings()
{
	QString Lang = ui.uiLang->currentData().toString();
	theConf->SetValue("Options/UiLanguage", Lang);
	quint32 LangId = LocaleNameToLCID(Lang.toStdWString().c_str(), 0);

	theConf->SetValue("UIConfig/UIFont", ui.lblUiFont->text());
	theConf->SetValue("Options/DPIScaling", ui.cmbDPI->currentData());

	theConf->SetValue("Options/UseDarkTheme", CSettingsWindow__Chk2Int(ui.chkDarkTheme->checkState()));
	theConf->SetValue("Options/UseFusionTheme", CSettingsWindow__Chk2Int(ui.chkFusionTheme->checkState()));
	theConf->SetValue("Options/AltRowColors", ui.chkAltRows->isChecked());
	theConf->SetValue("Options/UseBackground", CSettingsWindow__Chk2Int(ui.chkBackground->checkState()));
	theConf->SetValue("Options/LargeIcons", CSettingsWindow__Chk2Int(ui.chkLargeIcons->checkState()));
	theConf->SetValue("Options/NoIcons", CSettingsWindow__Chk2Int(ui.chkNoIcons->checkState()));
	theConf->SetValue("Options/OptionTree", CSettingsWindow__Chk2Int(ui.chkOptTree->checkState()));
	theConf->SetValue("Options/NewConfigLayout", CSettingsWindow__Chk2Int(ui.chkNewLayout->checkState()));
	theConf->SetValue("Options/ColorBoxIcons", ui.chkColorIcons->isChecked());
	theConf->SetValue("Options/UseOverlayIcons", ui.chkOverlayIcons->isChecked());
	theConf->SetValue("Options/HideSbieProcesses", ui.chkHideCore->isChecked());
	theConf->SetValue("Options/HighlightPendingChanges", CSettingsWindow__Chk2Int(ui.chkHighlightPendingChanges->checkState()));
	theConf->SetValue("Options/BoxGroupHandling", ui.cmbGrouping->currentIndex());

	CIniHighlighter::ClearLanguageCache();
	CIniHighlighter::ClearThemeCache();

	int Scaling = ui.cmbFontScale->currentText().toInt();
	if (Scaling < 75)
		Scaling = 75;
	else if (Scaling > 500)
		Scaling = 500;
	theConf->SetValue("Options/FontScaling", Scaling);
	
	theConf->SetValue("Options/CoverWindows", ui.chkHide->isChecked());

	theConf->SetValue("Options/Editor", ui.txtEditor->text());

	AutorunEnable(ui.chkAutoStart->isChecked());

	if (theAPI->IsConnected()) {
		if (ui.chkSvcStart->checkState() == Qt::Checked) {
			theAPI->GetUserSettings()->SetBool("SbieCtrl_EnableAutoStart", true);
			theAPI->GetUserSettings()->SetText("SbieCtrl_AutoStartAgent", "SandMan.exe -autorun");
		}
		else if (ui.chkSvcStart->checkState() == Qt::Unchecked)
			theAPI->GetUserSettings()->SetBool("SbieCtrl_EnableAutoStart", false);

		theAPI->SbieIniSet("$", "Language", QString::number(LangId), CSbieIni::eIniUpdate, false);
		//theAPI->GetGlobalSettings()->SetNum("SbieSvc_Language", LangId);
	}

	if (ui.chkShellMenu->checkState() != IsContextMenu())
	{
		if (ui.chkShellMenu->isChecked()) {
			CSecretCheckBox* SecretCheckBox = qobject_cast<CSecretCheckBox*>(ui.chkShellMenu);
			AddContextMenu(SecretCheckBox && SecretCheckBox->IsSecretSet());
		}
		else
			RemoveContextMenu();
	}

	if (ui.chkShellMenu2->isChecked() != CSbieUtils::HasContextMenu2()) {
		if (ui.chkShellMenu2->isChecked()) {
			CSbieUtils::AddContextMenu2(QApplication::applicationDirPath().replace("/", "\\") + "\\SandMan.exe",
				tr("Run &Un-Sandboxed"),
				QApplication::applicationDirPath().replace("/", "\\") + "\\Start.exe");
		} else
			CSbieUtils::RemoveContextMenu2();
	}
	if (ui.chkShellMenu3->isChecked() != CSbieUtils::HasContextMenu3()) {
		if (ui.chkShellMenu3->isChecked()) {
			CSbieUtils::AddContextMenu3(QApplication::applicationDirPath().replace("/", "\\") + "\\SandMan.exe",
				tr("Set Force in Sandbox")/*,
				QApplication::applicationDirPath().replace("/", "\\") + "\\Start.exe"*/);
		}
		else
			CSbieUtils::RemoveContextMenu3();
	}
	if (ui.chkShellMenu4->isChecked() != CSbieUtils::HasContextMenu4()) {
		if (ui.chkShellMenu4->isChecked()) {
			CSbieUtils::AddContextMenu4(QApplication::applicationDirPath().replace("/", "\\") + "\\SandMan.exe",
				tr("Set Open Path in Sandbox")/*,
				QApplication::applicationDirPath().replace("/", "\\") + "\\Start.exe"*/);
		}
		else
			CSbieUtils::RemoveContextMenu4();
	}
	theConf->SetValue("Options/RunInDefaultBox", ui.chkAlwaysDefault->isChecked());
	theConf->SetValue("Options/RememberLastBox", ui.chkRememberLast->isChecked());

	theConf->SetValue("Options/CheckSilentMode", ui.chkSilentMode->isChecked());
	theConf->SetValue("Options/ShowMigrationProgress", ui.chkCopyProgress->isChecked());
	theConf->SetValue("Options/ShowNotifications", !ui.chkNoMessages->isChecked());

	theConf->SetValue("Options/OpenUrlsSandboxed", CSettingsWindow__Chk2Int(ui.chkSandboxUrls->checkState()));
	theConf->SetValue("Options/TerminateWhenExit", ui.chkAutoTerminate->isChecked());

	theConf->SetValue("Options/ShowRecovery", ui.chkShowRecovery->isChecked());
	theConf->SetValue("Options/InstantRecovery", !ui.chkNotifyRecovery->isChecked());
	theConf->SetValue("Options/RecoveryOnTop", ui.chkRecoveryTop->isChecked());
	theConf->SetValue("Options/UseAsyncBoxOps", ui.chkAsyncBoxOps->isChecked());

	theConf->SetValue("Options/EnablePanicKey", ui.chkPanic->isChecked());
	theConf->SetValue("Options/PanicKeySequence", ui.keyPanic->keySequence().toString());

	theConf->SetValue("Options/EnableTopMostKey", ui.chkTop->isChecked());
	theConf->SetValue("Options/TopMostKeySequence", ui.keyTop->keySequence().toString());

	theConf->SetValue("Options/EnablePauseForceKey", ui.chkPauseForce->isChecked());
	theConf->SetValue("Options/PauseForceKeySequence", ui.keyPauseForce->keySequence().toString());

	theConf->SetValue("Options/EnableSuspendKey", ui.chkSuspend->isChecked());
	theConf->SetValue("Options/SuspendKeySequence", ui.keySuspend->keySequence().toString());

	theConf->SetValue("Options/CleanUpOnStart", ui.chkCheckDelete->isChecked());

	theConf->SetValue("Options/WatchBoxSize", ui.chkMonitorSize->isChecked());

	theConf->SetValue("Options/WatchIni", ui.chkWatchConfig->isChecked());
	if (m_SkipUACChanged)
		SkipUacEnable(ui.chkSkipUAC->isChecked());

	theConf->SetValue("Options/ScanStartMenu", ui.chkScanMenu->isChecked());
	int OldIntegrateStartMenu = theConf->GetInt("Options/IntegrateStartMenu", 0);
	theConf->SetValue("Options/IntegrateStartMenu", ui.cmbIntegrateMenu->currentIndex());
	int OldIntegrateDesktop = theConf->GetInt("Options/IntegrateDesktop", 0);
	theConf->SetValue("Options/IntegrateDesktop", ui.cmbIntegrateDesk->currentIndex());
	if (ui.cmbIntegrateDesk->currentIndex() != OldIntegrateDesktop || ui.cmbIntegrateMenu->currentIndex() != OldIntegrateStartMenu) {
		theGUI->ClearStartMenu();
		theGUI->SyncStartMenu();
	}

#ifdef INSIDER_BUILD
	theConf->SetValue("Options/AutoDesktopSwitch", ui.chkDeskAutoSwitch->isChecked());
	theConf->SetValue("Options/QuickDesktopSwitch", ui.chkDeskQuickSwitch->isChecked());
#endif

	theConf->SetValue("Options/SysTrayIcon", ui.cmbSysTray->currentIndex());
	theConf->SetValue("Options/SysTrayFilter", ui.cmbTrayBoxes->currentIndex());
	theConf->SetValue("Options/CompactTray", ui.chkCompactTray->isChecked());
	theConf->SetValue("Options/TraySearch", ui.chkTraySearch->isChecked());
	theConf->SetValue("Options/TrayIcons", ui.chkTrayIcons->isChecked());
	theConf->SetValue("Options/TrayOverlayIcons", ui.chkTrayOverlayIcons->isChecked());
	theConf->SetValue("Options/TrayUseAlias", ui.chkTrayUseAlias->isChecked());
	theConf->SetValue("Options/TrayAliasMaxChars", ui.spnTrayAliasChars->value());
	theConf->SetValue("Options/TrayStatusTip", (int)ui.chkTrayStatusTip->checkState());
	theConf->SetValue("Options/TrayStatusTipModifier", ui.cmbTrayStatusTipModifier->currentData());
	theConf->SetValue("Options/AutoBoxOpsNotify", ui.chkBoxOpsNotify->isChecked());
	theConf->SetValue("Options/OnClose", ui.cmbOnClose->currentData());
	theConf->SetValue("Options/MinimizeToTray", ui.chkMinimize->isChecked());
	auto getPersistedMonitorSetting = [&](QComboBox* pCombo, const QString& settingName)
	{
		int selectedValue = pCombo->currentData().toInt();
		if (m_WindowMonitorResolvedFallback.contains(settingName) && m_WindowMonitorRawSettings.contains(settingName)) {
			int autoResolvedFallbackValue = m_WindowMonitorResolvedFallback.value(settingName);
			if (selectedValue == autoResolvedFallbackValue)
				return m_WindowMonitorRawSettings.value(settingName);
		}
		return selectedValue;
	};

	int mainMonitorSetting = getPersistedMonitorSetting(ui.cmbLaunchMonitor, "Options/WindowTargetMonitor");
	int nonMainMonitorSetting = getPersistedMonitorSetting(ui.cmbNonMainLaunchMonitor, "Options/NonMainWindowTargetMonitor");
	int recoveryMonitorSetting = getPersistedMonitorSetting(ui.cmbRecoveryLaunchMonitor, "Options/RecoveryWindowTargetMonitor");
	int notificationMonitorSetting = getPersistedMonitorSetting(ui.cmbNotificationLaunchMonitor, "Options/NotificationWindowTargetMonitor");
	theConf->SetValue("Options/WindowTargetMonitor", mainMonitorSetting);
	theConf->SetValue("Options/NonMainWindowTargetMonitor", nonMainMonitorSetting);
	theConf->SetValue("Options/RecoveryWindowTargetMonitor", recoveryMonitorSetting);
	theConf->SetValue("Options/NotificationWindowTargetMonitor", notificationMonitorSetting);
	theConf->SetValue("Options/WindowMonitorFallback", ui.cmbFallbackActiveMonitor->currentData().toInt());
	theConf->SetValue("Options/TraySingleClick", ui.chkSingleShow->isChecked());

	theConf->SetValue("Options/UseW11Style", ui.chkUseW11Style->isChecked());
	theConf->SetValue("Options/UseRandomBoxName", ui.chkRandomGuidName->isChecked());

	if (theAPI->IsConnected())
	{
		try
		{
			if (m_MessagesChanged)
			{
				m_MessagesChanged = false;
				
				QStringList HiddenMessages;
				for (int i = 0; i < ui.treeMessages->topLevelItemCount(); i++) {
					QTreeWidgetItem* pItem = ui.treeMessages->topLevelItem(i);
					if (!pItem->text(1).isEmpty())
						HiddenMessages.append(pItem->text(0) + "," + pItem->text(1));
					else
						HiddenMessages.append(pItem->text(0));
				}
				theAPI->GetUserSettings()->UpdateTextList("SbieCtrl_HideMessage", HiddenMessages, false);
			}

			if (m_RunChanged) 
			{
				m_RunChanged = false;

				QStringList RunCommands;
				for (int i = 0; i < ui.treeRun->topLevelItemCount(); i++) 
					RunCommands.prepend(MakeRunEntry(ui.treeRun->topLevelItem(i)));

				//WriteTextList("RunCommand", RunCommands);
				theAPI->GetGlobalSettings()->DelValue("RunCommand");
				foreach(const QString & Value, RunCommands)
					theAPI->GetGlobalSettings()->AppendText("RunCommand", Value);
			}

			if (m_GeneralChanged)
			{
				m_GeneralChanged = false;

				WriteText("DefaultBox", ui.cmbDefault->currentData().toString());

				WriteText("FileRootPath", ui.fileRoot->currentText()); //ui.fileRoot->setText("\\??\\%SystemDrive%\\Sandbox\\%USER%\\%SANDBOX%");
				WriteAdvancedCheck(ui.chkLockBox, "LockBoxToUser", "y", "");
				//WriteAdvancedCheck(ui.chkSeparateUserFolders, "SeparateUserFolders", "", "n");
				WriteText("KeyRootPath", ui.regRoot->currentText()); //ui.regRoot->setText("\\REGISTRY\\USER\\Sandbox_%USER%_%SANDBOX%");
				WriteText("IpcRootPath", ui.ipcRoot->currentText()); //ui.ipcRoot->setText("\\Sandbox\\%USER%\\%SANDBOX%\\Session_%SESSION%");

				WriteText("RamDiskSizeKb", ui.chkRamDisk->isChecked() ? ui.txtRamLimit->text() : "");
				WriteText("RamDiskLetter", ui.chkRamLetter->isChecked() ? ui.cmbRamLetter->currentText() : "");

				WriteAdvancedCheck(ui.chkWFP, "NetworkEnableWFP", "y", "");
				WriteAdvancedCheck(ui.chkObjCb, "EnableObjectFiltering", "", "n");
				WriteAdvancedCheck(ui.chkWin32k, "EnableWin32kHooks", "", "n");
				WriteAdvancedCheck(ui.chkSbieLogon, "SandboxieLogon", "y", "");
				WriteAdvancedCheck(ui.chkSbieAll, "SandboxieAllGroup", "", "n");
				WriteAdvancedCheck(ui.chkSbieUAC, "UseSandboxieUAC", "", "n");
				WriteText("BoxAliasDisplayMode", ui.cmbBoxAliasDisplayMode->currentData().toInt() == 2
					? QString() : QString::number(ui.cmbBoxAliasDisplayMode->currentData().toInt()));

				if (m_FeaturesChanged) {
					m_FeaturesChanged = false;
					theAPI->ReloadConfig(true);
				}
			}

			if (m_ImportChanged)
			{
				m_ImportChanged = false;

				QStringList Imports;
				for (int i = 0; i < ui.treeImport->topLevelItemCount(); i++)
					Imports.append(ui.treeImport->topLevelItem(i)->text(0));

				WriteTextList("ImportBox", Imports);
			}

			if (m_ProtectionChanged)
			{
				m_ProtectionChanged = false;

				WriteAdvancedCheck(ui.chkAdminOnly, "EditAdminOnly", "y", "");

				bool isPassSet = !theAPI->GetGlobalSettings()->GetText("EditPassword", "").isEmpty();
				if (ui.chkPassRequired->isChecked())
				{
					if (!isPassSet && m_NewPassword.isEmpty())
						OnSetPassword(); // request password entry if it wasn't entered already
					if (!m_NewPassword.isEmpty()) {
						theAPI->LockConfig(m_NewPassword); // set new/changed password
						m_NewPassword.clear();
					}
				}
				else if (isPassSet)
					theAPI->LockConfig(QString()); // clear password

				WriteAdvancedCheck(ui.chkAdminOnlyFP, "ForceDisableAdminOnly", "y", "");
				WriteAdvancedCheck(ui.chkClearPass, "ForgetPassword", "y", "");
			}

			if (m_WarnProgsChanged)
			{
				m_WarnProgsChanged = false;

				WriteAdvancedCheck(ui.chkStartBlock, "StartRunAlertDenied", "y", "");
				WriteAdvancedCheck(ui.chkStartBlockMsg, "AlertStartRunAccessDenied", "", "n");
				WriteAdvancedCheck(ui.chkNotForcedMsg, "NotifyForceProcessDisabled", "y", "");
				WriteAdvancedCheck(ui.chkForcedMsg, "NotifyForceProcessEnabled", "y", "");

				QStringList AlertProcess;
				QStringList AlertFolder;
				for (int i = 0; i < ui.treeWarnProgs->topLevelItemCount(); i++)
				{
					QTreeWidgetItem* pItem = ui.treeWarnProgs->topLevelItem(i);
					int Type = pItem->data(0, Qt::UserRole).toInt();
					switch (Type)
					{
					case 1:	AlertProcess.append(pItem->data(1, Qt::UserRole).toString()); break;
					case 2: AlertFolder.append(pItem->data(1, Qt::UserRole).toString()); break;
					}
				}

				WriteTextList("AlertProcess", AlertProcess);
				WriteTextList("AlertFolder", AlertFolder);
			}

			WriteAdvancedCheck(ui.chkSandboxMoTW, "ForceMarkOfTheWeb", "y", "");
			WriteAdvancedCheck(ui.chkForceBoxDocs, "ForceBoxDocs", "y", "");

			QString MoTWSandbox = ui.cmbMoTWSandbox->currentData().toString();
			SB_STATUS Status = theAPI->ValidateName(MoTWSandbox);
			if (Status.IsError())
				QMessageBox::warning(this, "Sandboxie-Plus", theGUI->FormatError(Status));
			else
				WriteText("MarkOfTheWebBox", MoTWSandbox);

			if (m_VolumeChanged)
			{
				m_VolumeChanged = false;

				WriteAdvancedCheck(ui.chkSandboxUsb, "ForceUsbDrives", "y", "");

				QString UsbSandbox = ui.cmbUsbSandbox->currentData().toString();
				SB_STATUS Status = theAPI->ValidateName(UsbSandbox);
				if (Status.IsError())
					QMessageBox::warning(this, "Sandboxie-Plus", theGUI->FormatError(Status));
				else
					WriteText("UsbSandbox", UsbSandbox);

				QStringList DisabledForceVolume;
				for (int i = 0; i < ui.treeVolumes->topLevelItemCount(); i++) {
					QTreeWidgetItem* pItem = ui.treeVolumes->topLevelItem(i);
					if (pItem->checkState(0) == Qt::Unchecked) {
						DisabledForceVolume.append(pItem->text(0));
					}
				}

				WriteTextList("DisabledForceVolume", DisabledForceVolume);

				theGUI->UpdateForceUSB();
			}

			if (m_CompatChanged)
			{
				m_CompatChanged = false;

				QStringList Used;
				QStringList Rejected;
				for (int i = 0; i < ui.treeCompat->topLevelItemCount(); i++) {
					QTreeWidgetItem* pItem = ui.treeCompat->topLevelItem(i);
					QString Template = pItem->data(0, Qt::UserRole).toString().mid(9);
					if (pItem->checkState(0) == Qt::Unchecked)
						Rejected.append(Template);
					else
						Used.append(Template);
				}

				// retain local templates
				foreach(const QString& Template, theAPI->GetGlobalSettings()->GetTextList("Template", false)) {
					if (Template.left(6) == "Local_") {
						Used.append(Template);
					}
				}

				WriteTextList("Template", Used);
				WriteTextList("TemplateReject", Rejected);
			}
		}
		catch (SB_STATUS Status)
		{
			theGUI->CheckResults(QList<SB_STATUS>() << Status, theGUI);
		}
	}

	if (ui.chkAutoRoot->isVisible())
		theConf->SetValue("Options/PortableRootDir", CSettingsWindow__Chk2Int(ui.chkAutoRoot->checkState()));

	theConf->SetValue("Options/AutoRunSoftCompat", !ui.chkNoCompat->isChecked());

	//theConf->SetValue("Options/CheckForTemplates", CSettingsWindow__Chk2Int(ui.chkUpdateTemplates->checkState()));
	theConf->SetValue("Options/CheckForAddons", CSettingsWindow__Chk2Int(ui.chkUpdateAddons->checkState()));
	theConf->SetValue("Options/CheckForIssues", CSettingsWindow__Chk2Int(ui.chkUpdateIssues->checkState()));

	emit OptionsChanged(m_bRebuildUI);
}


void CSettingsWindow::apply()
{
	if (!ui.btnEditIni->isEnabled())
		SaveIniSection();
	else
		SaveSettings();
	LoadSettings();
}

void CSettingsWindow::ok()
{
	apply();

	this->close();
}

void CSettingsWindow::reject()
{
	this->close();
}

void CSettingsWindow::OnOptChanged()
{
	QStandardItemModel *model = qobject_cast<QStandardItemModel *>(ui.cmbOnClose->model());

	QStandardItem *item = model->item(0);
	item->setFlags((ui.cmbSysTray->currentIndex() == 0) ? item->flags() & ~Qt::ItemIsEnabled : item->flags() | Qt::ItemIsEnabled);

	item = model->item(3);
	item->setFlags((ui.cmbSysTray->currentIndex() != 0) ? item->flags() & ~Qt::ItemIsEnabled : item->flags() | Qt::ItemIsEnabled);

	if (sender() == ui.chkHighlightPendingChanges) {
		m_PendingChanges.SetEnabled(CSettingsWindow__IsPendingHighlightEnabled(ui.chkHighlightPendingChanges->checkState()), m_pTree);
		if (!m_HoldChange)
			m_PendingChanges.UpdateAll(m_pTree);
	}
	else if (!m_HoldChange)
		m_PendingChanges.Update(sender(), m_pTree);

	if (m_HoldChange)
		return;
	ui.buttonBox->button(QDialogButtonBox::Apply)->setEnabled(true);
}

void CSettingsWindow::OnLoadAddon()
{
	ui.treeAddons->clear();
	foreach(const CAddonInfoPtr pAddon, theGUI->GetAddonManager()->GetAddons()) {

		QTreeWidgetItem* pItem = new QTreeWidgetItem;
		pItem->setText(0, pAddon->GetLocalizedEntry("name"));
		if(!pAddon->Data["mandatory"].toBool())
			pItem->setData(0, Qt::UserRole, pAddon->Id);
		pItem->setIcon(0, pAddon->Data.contains("icon") ? CSandMan::GetIcon(pAddon->Data["icon"].toString()) : CSandMan::GetIcon("Addon"));
		if (pAddon->Installed) {
			if(!pAddon->UpdateVersion.isEmpty())
				pItem->setText(1, tr("Update Available"));
			else
				pItem->setText(1, tr("Installed"));
		}
		pItem->setText(2, pAddon->Data["version"].toString());
		QString Maker = pAddon->Data["maintainer"].toString();
		if(!Maker.isEmpty())
			pItem->setToolTip(2, tr("by %1").arg(Maker));
		//pItem->setText(3, );

		ui.treeAddons->addTopLevelItem(pItem);

		QString Info = pAddon->GetLocalizedEntry("description");

		QString infoUrl = pAddon->Data["infoUrl"].toString();
		if (!infoUrl.isEmpty()) Info += " <a href=\"" + infoUrl + "\">" + tr("(info website)") + "</a>";
		QLabel* pLabel = new QLabel(Info);
		//pLabel->setToolTip(tr("by %1").arg(pAddon->Data["maintainer"].toString()));
		connect(pLabel, SIGNAL(linkActivated(const QString&)), theGUI, SLOT(OpenUrl(const QString&)));
		ui.treeAddons->setItemWidget(pItem, 3, pLabel);
	}
	m_PendingChanges.CaptureItemBaselines(m_pTree, ui.treeAddons);
}

void CSettingsWindow::OnInstallAddon()
{
	QTreeWidgetItem* pItem = ui.treeAddons->currentItem();
	if (!pItem)
		return;

	QString Id = pItem->data(0, Qt::UserRole).toString();
	SB_PROGRESS Status = theGUI->GetAddonManager()->TryInstallAddon(Id, this);
	if (Status.GetStatus() == OP_ASYNC) connect(Status.GetValue().data(), SIGNAL(Finished()), this, SLOT(OnLoadAddon()));
}

void CSettingsWindow::OnRemoveAddon()
{
	QTreeWidgetItem* pItem = ui.treeAddons->currentItem();
	if (!pItem)
		return;
	
	QString Id = pItem->data(0, Qt::UserRole).toString();
	if (Id.isEmpty()) {
		QMessageBox::warning(this, "Sandboxie-Plus", tr("This Add-on is mandatory and can not be removed."));
		return;
	}
	SB_PROGRESS Status = theGUI->GetAddonManager()->TryRemoveAddon(Id, this);
	if (Status.GetStatus() == OP_ASYNC) connect(Status.GetValue().data(), SIGNAL(Finished()), this, SLOT(OnLoadAddon()));
}

void CSettingsWindow::OnBrowse()
{
	QString Value = QFileDialog::getExistingDirectory(this, tr("Select Directory")).replace("/", "\\");
	if (Value.isEmpty())
		return;

	ui.fileRoot->setCurrentText(Value + "\\%SANDBOX%");
}

void CSettingsWindow::OnRootChanged()
{
	if (ui.chkAutoRoot->isVisible())
		ui.fileRoot->setEnabled(ui.chkAutoRoot->checkState() != Qt::Checked);
	OnOptChanged();
}

void CSettingsWindow::OnTab()
{
	OnTab(ui.tabs->currentWidget());
}

void CSettingsWindow::OnTab(QWidget* pTab)
{
	m_pCurrentTab = pTab;

	if (pTab == ui.tabAddons)
	{
		if (theConf->GetInt("Options/CheckForAddons", 2) == 1) {
			ui.lblUpdateAddons->setVisible(false);
			theGUI->GetAddonManager()->UpdateAddonsWhenNotCached();
		}
	}
	else if (pTab == ui.tabEdit)
	{
		LoadIniSection();
		//ui.txtIniSection->setReadOnly(true);
	}
	else if (pTab == ui.tabCompat && m_CompatLoaded != 1 && theAPI->IsConnected())
	{
		if(m_CompatLoaded == 0)
			theGUI->CheckCompat(this, "OnCompat");
	}
}

void CSettingsWindow::OnCompat()
{
	ui.treeCompat->clear();

	bool bNew = false;

	QMap<QString, int> Templates = theGUI->GetCompat()->GetTemplates();
	for (QMap<QString, int>::iterator I = Templates.begin(); I != Templates.end(); ++I)
	{
		if (I.value() == CSbieTemplates::eNone)
			continue;

		QSharedPointer<CSbieIni> pTemplate = QSharedPointer<CSbieIni>(new CSbieIni("Template_" + I.key(), theAPI));

		QString Title = pTemplate->GetText("Tmpl.Title", "", false, true, true);
		if (Title.left(1) == "#")
		{
			int End = Title.mid(1).indexOf(",");
			if (End == -1) End = Title.length() - 1;
			int MsgNum = Title.mid(1, End).toInt();
			Title = theAPI->GetSbieMsgStr(MsgNum, theGUI->m_LanguageId).arg(Title.mid(End + 2)).arg("");
		}
		//if (Title.isEmpty()) Title = Name;

		QTreeWidgetItem* pItem = new QTreeWidgetItem();
		pItem->setText(0, Title);
		pItem->setData(0, Qt::UserRole, "Template_" + I.key());
		if((I.value() & CSbieTemplates::eDisabled) != 0)
			pItem->setCheckState(0, Qt::Unchecked);
		else if((I.value() & CSbieTemplates::eEnabled) != 0)
			pItem->setCheckState(0, Qt::Checked);
		else {
			pItem->setCheckState(0, Qt::PartiallyChecked);
			bNew = true;
		}
		ui.treeCompat->addTopLevelItem(pItem);
	}

	m_CompatLoaded = 1;
	m_PendingChanges.CaptureItemBaselines(m_pTree, ui.treeCompat);
	if(bNew)
		OnCompatChanged();

	LoadTemplates();
}

void CSettingsWindow::OnImportBox()
{
	QString Value = QFileDialog::getOpenFileName(this, tr("Select Portable Box ini"), "", tr("Ini Files (*.ini)")).replace("/", "\\");
	if (Value.isEmpty())
		return;

	ui.treeImport->addTopLevelItem(new QTreeWidgetItem(QStringList() << Value));
	OnImportChanged();
}

void CSettingsWindow::OnMakeBox()
{
	QString Value = QFileDialog::getSaveFileName(this, tr("Save new Portable Box ini"), "", tr("Ini Files (*.ini)")).replace("/", "\\");
	if (Value.isEmpty())
		return;

	if (QFile::exists(Value)) {
		QMessageBox::critical(this, "Sandboxie-Plus", tr("File already exists, please select a different file name."));
		return;
	}

	QString Name = Split2(Split2(Value, "\\", true).second, ".").first.replace(" ", "_");
	if (theAPI->ValidateName(Name).IsError()) {
		QMessageBox::critical(this, "Sandboxie-Plus", tr("Invalid box name"));
		return;
	}

	QFile File(Value);
	if (File.open(QFile::WriteOnly)) {
		File.write("#\n");
		File.write("# Portable sandbox configuration file\n");
		File.write("#\n");
		File.write("\n");
		File.write("[" +  Name.toLatin1() + "]\n");
		File.write("Enabled=y\n");
		File.close();
	}

	ui.treeImport->addTopLevelItem(new QTreeWidgetItem(QStringList() << Value));
	OnImportChanged();
}

void CSettingsWindow::OnAddRoot()
{
	QString Value = QFileDialog::getExistingDirectory(this, tr("Select Root Folder")).replace("/", "\\");
	if (Value.isEmpty())
		return;

	ui.treeImport->addTopLevelItem(new QTreeWidgetItem(QStringList() << Value + "\\*"));
	OnImportChanged();
}

void CSettingsWindow::OnRemoveBox()
{
	QTreeWidgetItem* pItem = ui.treeImport->currentItem();
	if (!pItem)
		return;

	delete pItem;
	OnImportChanged();
}

void CSettingsWindow::OnProtectionChange()
{
	ui.btnSetPassword->setEnabled(ui.chkPassRequired->isChecked());
	m_ProtectionChanged = true;
	OnOptChanged();
}

void CSettingsWindow::OnSetPassword()
{
retry:
	QString Value1 = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter the new configuration password."), QLineEdit::Password);
	if (Value1.isEmpty())
		return;

	QString Value2 = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please re-enter the new configuration password."), QLineEdit::Password);
	if (Value2.isEmpty())
		return;

	if (Value1 != Value2) {
		QMessageBox::warning(this, "Sandboxie-Plus", tr("Passwords did not match, please retry."));
		goto retry;
	}

	m_NewPassword = Value1;
	m_ProtectionChanged = true;
	OnOptChanged();
}

void CSettingsWindow::AddWarnEntry(const QString& Name, int type)
{
	QTreeWidgetItem* pItem = new QTreeWidgetItem();
	pItem->setText(0, (type == 1 ? tr("Process") : tr("Folder")));
	pItem->setData(0, Qt::UserRole, type);

	pItem->setData(1, Qt::UserRole, Name);
	pItem->setText(1, Name);
	ui.treeWarnProgs->addTopLevelItem(pItem);
}

void CSettingsWindow::OnAddWarnProg()
{
	QString Value = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter a program file name"));
	if (Value.isEmpty())
		return;
	AddWarnEntry(Value, 1);
	OnWarnChanged();
}

void CSettingsWindow::OnAddWarnFolder()
{
	QString Value = QFileDialog::getExistingDirectory(this, tr("Select Directory")).replace("/", "\\");
	if (Value.isEmpty())
		return;
	AddWarnEntry(Value, 2);
	OnWarnChanged();
}

void CSettingsWindow::OnDelWarnProg()
{
	QTreeWidgetItem* pItem = ui.treeWarnProgs->currentItem();
	if (!pItem)
		return;

	delete pItem;
	OnWarnChanged();
}

void CSettingsWindow::OnTemplateClicked(QTreeWidgetItem* pItem, int Column)
{
	if (sender() == ui.treeCompat) {
		// todo: check if really changed
		OnCompatChanged();
	}
}

void CSettingsWindow::OnAddCompat()
{
	QTreeWidgetItem* pItem = ui.treeCompat->currentItem();
	if (!pItem)
		return;

	pItem->setCheckState(0, Qt::Checked);
	OnCompatChanged();
}

void CSettingsWindow::OnDelCompat()
{
	QTreeWidgetItem* pItem = ui.treeCompat->currentItem();
	if (!pItem)
		return;

	pItem->setCheckState(0, Qt::Unchecked);
	OnCompatChanged();
}

void CSettingsWindow::OnTemplateDoubleClicked(QTreeWidgetItem* pItem, int Column)
{
	QSharedPointer<CSbieIni> pTemplate = QSharedPointer<CSbieIni>(new CSbieIni(pItem->data(0, Qt::UserRole).toString(), theAPI));

	COptionsWindow OptionsWindow(pTemplate, pItem->text(0));
	QPoint ParentPos = mapToGlobal(rect().topLeft());
	OptionsWindow.move(ParentPos.x() + 30, ParentPos.y() + 10);
	OptionsWindow.exec();

	// todo update name if it changed
}

void CSettingsWindow::OnAddTemplates()
{
	QString Value = QInputDialog::getText(this, "Sandboxie-Plus", tr("Please enter the template identifier"), QLineEdit::Normal);
	if (Value.isEmpty())
		return;

	QString Name = QString(Value).replace(" ", "_");

	SB_STATUS Status = theAPI->ValidateName(Name);
	if (Status.IsError()) {
		QMessageBox::critical(this, "Sandboxie-Plus", tr("Error: %1").arg(CSandMan::FormatError(Status)));
		return;
	}

	QSharedPointer<CSbieIni> pTemplate = QSharedPointer<CSbieIni>(new CSbieIni("Template_Local_" + Name, theAPI));

	pTemplate->SetText("Tmpl.Title", Value);
	pTemplate->SetText("Tmpl.Class", "Local");

	COptionsWindow OptionsWindow(pTemplate, Value);
	OptionsWindow.exec();

	LoadTemplates();
}

void CSettingsWindow::OnTemplateWizard()
{
	CTemplateWizard::ETemplateType Type = (CTemplateWizard::ETemplateType)((QAction*)sender())->data().toInt();
	if (CTemplateWizard::CreateNewTemplate(NULL, Type, this)) {
		LoadTemplates();
	}
}

void CSettingsWindow::OnOpenTemplate()
{
	QTreeWidgetItem* pItem = ui.treeTemplates->currentItem();
	if (pItem)
		OnTemplateDoubleClicked(pItem, 0);
}

void CSettingsWindow::OnDelTemplates()
{
	if (QMessageBox("Sandboxie-Plus", tr("Do you really want to delete the selected local template(s)?"), QMessageBox::Question, QMessageBox::Yes, QMessageBox::No | QMessageBox::Default | QMessageBox::Escape, QMessageBox::NoButton, this).exec() != QMessageBox::Yes)
		return;

	foreach(QTreeWidgetItem * pItem, ui.treeTemplates->selectedItems())
	{
		QString Section = pItem->data(0, Qt::UserRole).toString();
		
		delete pItem;

		// delete section
		theAPI->SbieIniSet(Section, "*", "");
	}
}

void CSettingsWindow::LoadTemplates()
{
	QStringList Templates;
	for (int index = 0; ; index++)
	{
		QString Value = theAPI->SbieIniGet2("", "", index);
		if (Value.isNull())
			break;
		Templates.append(Value);
	}

	ui.treeTemplates->clear();

	QString TextFilter = ui.txtTemplates->text();

	foreach(const QString& Name, Templates)
	{
		if (Name.left(9).compare("Template_", Qt::CaseInsensitive) != 0)
			continue;

		if (Name.indexOf(TextFilter, 0, Qt::CaseInsensitive) == -1)
			continue;

		QString Title = theAPI->SbieIniGet2(Name, "Tmpl.Title");
		if (Title.left(1) == "#")
		{
			int End = Title.mid(1).indexOf(",");
			if (End == -1) End = Title.length() - 1;
			int MsgNum = Title.mid(1, End).toInt();
			Title = theAPI->GetSbieMsgStr(MsgNum, theGUI->m_LanguageId).arg(Title.mid(End + 2)).arg("");
		}
		if (Title.isEmpty()) Title = Name;

		QTreeWidgetItem* pItem = new QTreeWidgetItem();
		pItem->setData(0, Qt::UserRole, Name);
		pItem->setText(0, Title);
		ui.treeTemplates->addTopLevelItem(pItem);
	}

	m_PendingChanges.CaptureItemBaselines(m_pTree, ui.treeTemplates);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Raw section ini Editor
//

void CSettingsWindow::SetIniEdit(bool bEnable)
{
	if (m_pTree) {
		m_pTree->setEnabled(!bEnable);
	}
	else {
		for (int i = 4; i < ui.tabs->count() - 1; i++) {
			bool Enabled = ui.tabs->widget(i)->isEnabled();
			ui.tabs->setTabEnabled(i, !bEnable && Enabled);
			ui.tabs->widget(i)->setEnabled(Enabled);
		}
	}
	ui.btnSaveIni->setEnabled(bEnable);
	ui.btnCancelEdit->setEnabled(bEnable);
	//ui.txtIniSection->setReadOnly(!bEnable);
	ui.btnEditIni->setEnabled(!bEnable);
}

void CSettingsWindow::OnEditIni()
{
	SetIniEdit(true);
}

void CSettingsWindow::OnIniValidationToggled(int state)
{
	m_HoldChange = true;

	m_IniValidationEnabled = (state == Qt::Checked);
	
	// Only save to config if not in a reset-skip context
	if (!m_SkipSaveOnToggle) {
		theConf->SetValue("Options/ValidateIniKeys", m_IniValidationEnabled);
	}

	if (state == Qt::Unchecked) {
		CIniHighlighter::MarkSettingsDirty();
		CIniHighlighter::MarkUserSettingsDirty();
	}

	// Remove previous highlighter
	if (m_pIniHighlighter) {
		delete m_pIniHighlighter;
		m_pIniHighlighter = nullptr;
	}

	// Attach new highlighter to the code editor's document
	QTextEdit* pTextEdit = m_pCodeEdit->GetTextEdit();
	if (pTextEdit) {
		m_pIniHighlighter = new CIniHighlighter(theGUI->m_DarkTheme, pTextEdit->document(), m_IniValidationEnabled);
		m_pIniHighlighter->rehighlight();
		UpdateAutoCompletion();
	}

	m_HoldChange = false;
}

void CSettingsWindow::OnTooltipToggled(int state)
{
	m_HoldChange = true;

	// Only save to config if not in a reset-skip context
	if (!m_SkipSaveOnToggle) {
		theConf->SetValue("Options/EnableIniTooltips", state);
	}

	CIniHighlighter::SetTooltipMode(state);

	{
		int iniMode = theConf->GetInt("Options/EnableIniTooltips", static_cast<int>(CIniHighlighter::GetTooltipMode()));
		int popupMode = theConf->GetInt("Options/EnablePopupTooltips", iniMode);
		CCodeEdit::SetPopupTooltipsEnabled(popupMode);
	}

	if (state == Qt::Unchecked) {
		CIniHighlighter::ClearLanguageCache();
		CIniHighlighter::ClearThemeCache();
	}

	m_HoldChange = false;
}

void CSettingsWindow::OnAutoCompletionToggled(int state)
{
	m_HoldChange = true;

	// Show consent dialog if enabling and not yet consented
	if (state != Qt::Unchecked && !m_AutoCompletionConsent) {
		int chosenState = ShowConsentDialog();
		
		if (chosenState == Qt::Unchecked) {
			// Cancel - revert the checkbox and return
			ui.chkEnableAutoCompletion->setCheckState(Qt::Unchecked);
			m_HoldChange = false;
			return;
		}
		
		// Consent was given, update UI and state
		ui.chkEnableAutoCompletion->setEnabled(true);
		ui.chkEnableAutoCompletion->setTristate(true);
		ui.chkEnableAutoCompletion->setCheckState(static_cast<Qt::CheckState>(chosenState));
		state = chosenState;
	}

	// Only save to config if not in a reset-skip context
	if (!m_SkipSaveOnToggle) {
		theConf->SetValue("Options/EnableAutoCompletion", state);
	}

	CCodeEdit::SetAutoCompletionMode(state);

	m_HoldChange = false;
}

void CSettingsWindow::OnSaveIni()
{
	SaveIniSection();
	SetIniEdit(false);
	LoadSettings();
}

void CSettingsWindow::OnIniChanged()
{
	if (m_HoldChange)
		return;
	if(ui.btnEditIni->isEnabled())
		SetIniEdit(true);
	ui.buttonBox->button(QDialogButtonBox::Apply)->setEnabled(true);
}

void CSettingsWindow::OnCancelEdit()
{
	SetIniEdit(false);
	LoadIniSection();
}

void CSettingsWindow::LoadIniSection()
{
	QString Section;

	if(theAPI->IsConnected())
		Section = theAPI->SbieIniGetEx("GlobalSettings", "");

	m_HoldChange = true;
	//ui.txtIniSection->setPlainText(Section);
	m_pCodeEdit->SetCode(Section);
	// Update autocompletion after all settings are loaded
	UpdateAutoCompletion();
	m_HoldChange = false;
}

void CSettingsWindow::SaveIniSection()
{
	if(theAPI->IsConnected())
		//theAPI->SbieIniSet("GlobalSettings", "", ui.txtIniSection->toPlainText());
		theAPI->SbieIniSet("GlobalSettings", "", m_pCodeEdit->GetCode());

	//LoadIniSection();
}

QVariantMap GetRunEntry(const QString& sEntry)
{
	QVariantMap Entry;

	if (sEntry.left(1) == "{")
	{
		Entry = QJsonDocument::fromJson(sEntry.toUtf8()).toVariant().toMap();
	}
	else
	{
		StrPair NameCmd = Split2(sEntry, "|");
		StrPair NameIcon = Split2(NameCmd.first, ",");

		Entry["Name"] = NameIcon.first;
		Entry["Icon"] = NameIcon.second;
		Entry["Command"] = NameCmd.second;
	}

	return Entry;
}

void AddRunItem(QTreeWidget* treeRun, const QVariantMap& Entry)
{
	QTreeWidgetItem* pItem = new QTreeWidgetItem();
	pItem->setText(0, Entry["Name"].toString());
	pItem->setData(0, Qt::UserRole, Entry);
	pItem->setText(1, Entry["Command"].toString());
	pItem->setFlags(pItem->flags() | Qt::ItemIsEditable);
	treeRun->addTopLevelItem(pItem);
}

QString MakeRunEntry(QTreeWidgetItem* pItem)
{
	QVariantMap Entry = pItem->data(0, Qt::UserRole).toMap();
	Entry["Name"] = pItem->text(0);
	Entry["Command"] = pItem->text(1);
	return MakeRunEntry(Entry);
}

QString MakeRunEntry(const QVariantMap& Entry)
{
	if (!Entry["WorkingDir"].toString().isEmpty() || !Entry["Icon"].toString().isEmpty()) {
		QString workingDir = Entry["WorkingDir"].toString().replace("\"", ""); // Remove double quotes from WorkingDir
		QVariantMap cleanedEntry = Entry; // Make a copy of Entry
		cleanedEntry["WorkingDir"] = workingDir; // Update WorkingDir

		QJsonDocument doc(QJsonValue::fromVariant(cleanedEntry).toObject());
		QString sEntry = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
		return sEntry;
	} 
	//if(!Entry["Icon"].toString().isEmpty())
	//	return Entry["Name"].toString() + "," + Entry["Icon"].toString() + "|" + Entry["Command"].toString();
	return Entry["Name"].toString() + "|" + Entry["Command"].toString();
}

void WindowsMoveFile(const QString& From, const QString& To)
{
	std::wstring from = From.toStdWString();
	from.append(L"\0", 1);
	std::wstring to = To.toStdWString();
	to.append(L"\0", 1);

	SHFILEOPSTRUCTW SHFileOp;
    memset(&SHFileOp, 0, sizeof(SHFILEOPSTRUCT));
    SHFileOp.hwnd = NULL;
    SHFileOp.wFunc = To.isEmpty() ? FO_DELETE : FO_MOVE;
	SHFileOp.pFrom = from.c_str();
    SHFileOp.pTo = to.c_str();
    SHFileOp.fFlags = NULL;    

    //The Copying Function
    SHFileOperationW(&SHFileOp);
}

// Helper to load/save consent from config
void CSettingsWindow::LoadCompletionConsent()
{
	m_AutoCompletionConsent = theConf->GetBool("Options/AutoCompletionConsent", false);
}

void CSettingsWindow::SaveCompletionConsent()
{
	theConf->SetValue("Options/AutoCompletionConsent", m_AutoCompletionConsent);
}

QString CSettingsWindow::localizedCompletionShortcut()
{
	QKeySequence shortcut = QKeySequence(Qt::CTRL + Qt::Key_Space);
	return shortcut.toString(QKeySequence::NativeText); // Returns the localized shortcut
}

// Show consent dialog and return the chosen autocomplete state
// Returns: Qt::Unchecked (0) if cancelled, Qt::PartiallyChecked (1) for Basic, Qt::Checked (2) for Full
int CSettingsWindow::ShowConsentDialog()
{
	QMessageBox consentBox(this);
	consentBox.setWindowTitle(tr("Autocomplete Consent Required"));
	consentBox.setIcon(QMessageBox::Question);
	consentBox.setText(tr("Autocomplete feature requires your consent to proceed."));
	consentBox.setInformativeText(
		tr("If you are unsure about the settings displayed in the autocomplete popup, we strongly recommend consulting the software's documentation or source code before proceeding. Enabling this feature without proper understanding may lead to unintended consequences, for which you will be solely responsible.\n\n"
			"Choose autocomplete mode:\n"
			"%1 Manual: Autocomplete suggestions with %2.\n"
			"%1 While Typing: Autocomplete suggestions while typing.")
		.arg(QChar(0x2022))   // Bullet symbol
		.arg(localizedCompletionShortcut()) // Localized Ctrl+Space
	);

	QPushButton* basicButton = consentBox.addButton(tr("Manual"), QMessageBox::YesRole);
	basicButton->setToolTip(tr("Triggers autocomplete suggestions with %1.").arg(localizedCompletionShortcut()));

	QPushButton* fullButton = consentBox.addButton(tr("While Typing"), QMessageBox::YesRole);
	fullButton->setToolTip(tr("Triggers autocomplete suggestions while typing."));

	QPushButton* cancelButton = consentBox.addButton(tr("Cancel"), QMessageBox::NoRole);
	cancelButton->setToolTip(tr("Keeps autocomplete suggestions disabled."));

	consentBox.setDefaultButton(basicButton);
	
	consentBox.exec();
	QAbstractButton* clickedButton = consentBox.clickedButton();
	
	if (clickedButton == basicButton) {
		m_AutoCompletionConsent = true;
		SaveCompletionConsent();
		return Qt::PartiallyChecked; // Basic mode
	}
	else if (clickedButton == fullButton) {
		m_AutoCompletionConsent = true;
		SaveCompletionConsent();
		return Qt::Checked; // Full mode
	}
	else { // Cancel
		m_AutoCompletionConsent = false;
		SaveCompletionConsent();
		return Qt::Unchecked; // Cancelled
	}
}

void CSettingsWindow::OnEditorSettings()
{
	CEditorSettingsWindow editorWindow(this);
	if (editorWindow.exec() == QDialog::Accepted) {
		// Settings were saved by the dialog, now update the current UI to reflect changes
		bool previousConsent = m_AutoCompletionConsent;
		LoadCompletionConsent();
		bool newConsent = m_AutoCompletionConsent;
		
		// If consent was just granted (changed from false to true), show the consent dialog
		if (!previousConsent && newConsent) {
			int chosenState = ShowConsentDialog();
			
			// Save the chosen autocomplete mode to config
			theConf->SetValue("Options/EnableAutoCompletion", chosenState);
		}
		
		// Update the current checkboxes to reflect the new settings
		// Note: SettingsWindow only has UI checkboxes for 3 settings:
		// - ValidateIniKeys (ui.chkValidateIniKeys)
		// - EnableIniTooltips (ui.chkEnableTooltips)
		// - EnableAutoCompletion (ui.chkEnableAutoCompletion)
		// The other 4 settings (EnablePopupTooltips, EnableFuzzyMatching, AutoCompletionConsent,
		// ShowFutureSettings)
		// are managed by EditorSettings but don't have corresponding UI in SettingsWindow
		
		// Block signals while updating checkboxes to prevent toggle handlers from being called prematurely
		ui.chkValidateIniKeys->blockSignals(true);
		ui.chkEnableTooltips->blockSignals(true);
		ui.chkEnableAutoCompletion->blockSignals(true);
		
		// Read current values from config (will be defaults if settings were reset/deleted)
		bool defaultValidation = theConf->GetBool("Options/ValidateIniKeys", true);
		ui.chkValidateIniKeys->setChecked(defaultValidation);
		
		int defaultTooltip = theConf->GetInt("Options/EnableIniTooltips", 1); // 1 = BasicInfo
		ui.chkEnableTooltips->setCheckState(static_cast<Qt::CheckState>(defaultTooltip));
		
		int defaultAutoCompletion = theConf->GetInt("Options/EnableAutoCompletion", 0); // 0 = Disabled
		if (m_AutoCompletionConsent) { // Consented
			ui.chkEnableAutoCompletion->setTristate(true);
			ui.chkEnableAutoCompletion->setCheckState(static_cast<Qt::CheckState>(defaultAutoCompletion));
		}
		else {
			ui.chkEnableAutoCompletion->setTristate(false);
			ui.chkEnableAutoCompletion->setCheckState(Qt::Unchecked);
		}
		
		// Unblock signals before calling toggle handlers manually
		ui.chkValidateIniKeys->blockSignals(false);
		ui.chkEnableTooltips->blockSignals(false);
		ui.chkEnableAutoCompletion->blockSignals(false);
		
		// Apply the settings immediately
		// Set skip flag for reset settings to prevent re-saving them to config
		// For non-reset settings, allow normal save behavior
		
		// ValidateIniKeys
		m_SkipSaveOnToggle = editorWindow.WasValidateIniKeysReset();
		OnIniValidationToggled(defaultValidation ? Qt::Checked : Qt::Unchecked);
		m_SkipSaveOnToggle = false;
		
		// EnableIniTooltips
		m_SkipSaveOnToggle = editorWindow.WasEnableIniTooltipsReset();
		OnTooltipToggled(defaultTooltip);
		m_SkipSaveOnToggle = false;
		
		// EnableAutoCompletion
		m_SkipSaveOnToggle = editorWindow.WasEnableAutoCompletionReset();
		OnAutoCompletionToggled(defaultAutoCompletion);
		m_SkipSaveOnToggle = false;
		
		// Apply settings that don't have UI checkboxes in SettingsWindow
		// These are managed via EditorSettings only
		bool fuzzyEnabled = theConf->GetBool("Options/EnableFuzzyMatching", false);
		m_pCodeEdit->SetFuzzyMatchingEnabled(fuzzyEnabled);
		
		// Always update autocompletion list regardless of reset status
		UpdateAutoCompletion();
	}
}
