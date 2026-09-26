#include "stdafx.h"

#include "SetupWizard.h"
#include "../MiscHelpers/Common/Common.h"
#include "../Windows/SettingsWindow.h"
#include "../SandMan.h"
#include "Helpers/WinAdmin.h"
#include <QButtonGroup>
#include "../QSbieAPI/SbieUtils.h"
#include "../OnlineUpdater.h"

CSetupWizard::CSetupWizard(int iOldLevel, QWidget *parent)
    : QWizard(parent)
{
    if (iOldLevel < SETUP_LVL_1)
        setPage(Page_Intro, new CIntroPage);
    if (iOldLevel < SETUP_LVL_1) {
        setPage(Page_UI, new CUIPage);
        setPage(Page_Shell, new CShellPage);
        //setPage(Page_WFP, new CWFPPage);
    }
    if (iOldLevel < SETUP_LVL_2)
        setPage(Page_Update, new CSBUpdate);
    setPage(Page_Finish, new CFinishPage);

    setWizardStyle(ModernStyle);
    //setOption(HaveHelpButton, true);
    setPixmap(QWizard::LogoPixmap, QPixmap(":/SandMan.png").scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    connect(this, &QWizard::helpRequested, this, &CSetupWizard::showHelp);

    setWindowTitle(tr("Setup Wizard"));
}

void CSetupWizard::showHelp()
{
    static QString lastHelpMessage;

    QString message;

    switch (currentId()) {
    case Page_Intro:
        message = tr("The decision you make here will affect which page you get to see next.");
        break;
    default:
        message = tr("This help is likely not to be of any help.");
    }

    if (lastHelpMessage == message)
        message = tr("Sorry, I already gave all the help I could.");

    QMessageBox::information(this, tr("Setup Wizard Help"), message);

    lastHelpMessage = message;
}

bool CSetupWizard::ShowWizard(int iOldLevel)
{
    CSetupWizard wizard(iOldLevel, theGUI);
    if (!theGUI->SafeExec(&wizard))
        return false;
    
    if (iOldLevel < SETUP_LVL_1) {
        if (wizard.field("useAdvanced").toBool())
            theConf->SetValue("Options/ViewMode", 1);
        else if (wizard.field("useSimple").toBool())
            theConf->SetValue("Options/ViewMode", 0);
        else if (wizard.field("useClassic").toBool())
            theConf->SetValue("Options/ViewMode", 2);

        if (wizard.field("useBrightMode").toInt())
            theConf->SetValue("Options/UseDarkTheme", 0);
        else if (wizard.field("useDarkMode").toInt())
            theConf->SetValue("Options/UseDarkTheme", 1);

        AutorunEnable(wizard.field("isAutoStart").toBool());

        if (wizard.field("useContecxtMenu").toBool())
            CSettingsWindow::AddContextMenu();

        if (wizard.field("useBrowserIcon").toBool())
            CSettingsWindow::AddBrowserIcon();

        //if (wizard.field("useWFP").toBool()) {
        //    theAPI->GetGlobalSettings()->SetBool("NetworkEnableWFP", true);
        //    theAPI->ReloadConfig(true);
        //}
    }

    if (iOldLevel < SETUP_LVL_2) {
        if (wizard.field("updateIssues").toBool())
            theConf->SetValue("Options/CheckForIssues", 1);
        if (wizard.field("updateAddons").toBool())
            theConf->SetValue("Options/CheckForAddons", 1);
    }

    //if (wizard.field("isUpdate").toBool())
    //    theConf->SetValue("Options/CheckForUpdates", 1);

    if (iOldLevel < SETUP_LVL_1) 
    {
        if (wizard.field("editAdminOnly").toBool())
            theAPI->GetGlobalSettings()->SetText("EditAdminOnly", "y");
    }

    theConf->SetValue("Options/WizardLevel", SETUP_LVL_CURRENT);

    theGUI->UpdateSettings(true);
    
    return true;
}

void CSetupWizard::ShellUninstall()
{
    AutorunEnable(false);

	CSettingsWindow::RemoveContextMenu();
	CSbieUtils::RemoveContextMenu2();
	CSbieUtils::RemoveContextMenu3();
	CSbieUtils::RemoveContextMenu4();

    // todo: delete desktop browser shortcut and start menu integration
}

//////////////////////////////////////////////////////////////////////////////////////////
// CIntroPage
// 

CIntroPage::CIntroPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle(tr("Introduction"));
    QPixmap Logo = QPixmap(theGUI->m_DarkTheme ? ":/SideLogoDM.png" : ":/SideLogo.png");
    int Scaling = theConf->GetInt("Options/FontScaling", 100);
    if(Scaling !=  100) Logo = Logo.scaled(Logo.width() * Scaling / 100, Logo.height() * Scaling / 100);
    setPixmap(QWizard::WatermarkPixmap, Logo);

    QVBoxLayout *layout = new QVBoxLayout;
    QLabel* pTopLabel = new QLabel(tr("Welcome to the Setup Wizard. This wizard will help you to configure your copy of <b>Sandboxie-Plus</b>. "
        "You can start this wizard at any time from the Sandbox->Maintenance menu if you do not wish to complete it now."));
    pTopLabel->setWordWrap(true);
    layout->addWidget(pTopLabel);

    QWidget* pSpacer = new QWidget();
    pSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(pSpacer);

    setLayout(layout);

    if (theGUI->m_DarkTheme) {
        QPalette palette = this->palette();
        palette.setColor(QPalette::Base, QColor(53, 53, 53));
        this->setPalette(palette);
    }
}

int CIntroPage::nextId() const
{
    return CSetupWizard::Page_UI;
}

//////////////////////////////////////////////////////////////////////////////////////////
// CUIPage
//

CUIPage::CUIPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Configure <b>Sandboxie-Plus</b> UI"));
    setSubTitle(tr("Select the user interface style you prefer."));

    QGridLayout* layout = new QGridLayout;

    m_pAdvanced = new QRadioButton(tr("&Advanced UI for experts"));
    m_pAdvanced->setChecked(theConf->GetInt("Options/ViewMode", 1) == 1);
    layout->addWidget(m_pAdvanced, 0, 0);
    registerField("useAdvanced", m_pAdvanced);

    m_pSimple = new QRadioButton(tr("&Simple UI for beginners"));
    m_pSimple->setChecked(theConf->GetInt("Options/ViewMode", 1) == 0);
    layout->addWidget(m_pSimple, 1, 0);
    registerField("useSimple", m_pSimple);

    m_pClassic = new QRadioButton(tr("&Vintage SbieCtrl.exe UI"));
    m_pClassic->setChecked(theConf->GetInt("Options/ViewMode", 1) == 2);
    layout->addWidget(m_pClassic, 2, 0);
    registerField("useClassic", m_pClassic);

    QButtonGroup *buttonGroup1 = new QButtonGroup();
    buttonGroup1->addButton(m_pAdvanced, 0);
    buttonGroup1->addButton(m_pSimple, 1);
    buttonGroup1->addButton(m_pClassic, 2);
    connect(buttonGroup1, SIGNAL(buttonClicked(QAbstractButton*)), this, SLOT(UpdatePreview()));

    QLabel* pDummy = new QLabel();
    pDummy->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    layout->addWidget(pDummy, 0, 1, 6, 4);
    pDummy->setStyleSheet("QLabel { background-color : " + QApplication::palette().color(QPalette::Base).name() + "; }");

    m_pPreview = new QLabel();
    m_pPreview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_pPreview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(m_pPreview, 0, 1, 6, 4);

    QWidget* pSpacer = new QWidget();
	pSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(pSpacer, 3, 0);

    m_pBrightMode = new QRadioButton(tr("Use Bright Mode"));
    layout->addWidget(m_pBrightMode, 4, 0);
    registerField("useBrightMode", m_pBrightMode);

    m_pDarkMode = new QRadioButton(tr("Use Dark Mode"));
    layout->addWidget(m_pDarkMode, 5, 0);
    registerField("useDarkMode", m_pDarkMode);

    QButtonGroup *buttonGroup2 = new QButtonGroup();
    buttonGroup2->addButton(m_pBrightMode, 0);
    buttonGroup2->addButton(m_pDarkMode, 1);
    connect(buttonGroup2, SIGNAL(buttonClicked(QAbstractButton*)), this, SLOT(UpdatePreview()));



    layout->addItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum), 0, 1, 1, 1);
    layout->addItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum), 0, 2, 1, 1);
    layout->addItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum), 0, 3, 1, 1);
    layout->addItem(new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum), 0, 4, 1, 1);

    setLayout(layout);
}

void CUIPage::initializePage() 
{ 
    QTimer::singleShot(10, this, SLOT(UpdatePreview()));
}

void CUIPage::UpdatePreview()
{
    bool bDark;
    if (m_pDarkMode->isChecked())
        bDark = true;
    else if (m_pBrightMode->isChecked())
        bDark = false;
    else { // same as os
        QSettings settings("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", QSettings::NativeFormat);
        bDark = (settings.value("AppsUseLightTheme") == 0);
    }

    QPixmap preview;
    if(m_pAdvanced->isChecked() && !bDark)
        preview = QPixmap::fromImage(QImage(":/Assets/Advanced.png"));
    else if(m_pAdvanced->isChecked() && bDark)
        preview =  QPixmap::fromImage(QImage(":/Assets/AdvancedD.png"));
    else if(m_pSimple->isChecked() && !bDark)
        preview = QPixmap::fromImage(QImage(":/Assets/Simple.png"));
    else if(m_pSimple->isChecked() && bDark)
        preview = QPixmap::fromImage(QImage(":/Assets/SimpleD.png"));
    else if(m_pClassic->isChecked() && !bDark)
        preview = QPixmap::fromImage(QImage(":/Assets/Classic.png"));
    else if(m_pClassic->isChecked() && bDark)
        preview = QPixmap::fromImage(QImage(":/Assets/ClassicD.png"));

    //QRect rect(0, 0, m_pPreview->width(), m_pPreview->height());
    //m_pPreview->setPixmap(preview.scaled(preview.width()*5/10, preview.height()*5/10, Qt::KeepAspectRatio, Qt::SmoothTransformation).copy(rect));
    //m_pPreview->setPixmap(preview.scaled(min(m_pPreview->width(), preview.width()), min(m_pPreview->height(), preview.height()), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    //m_pPreview->setPixmap(preview);
    //m_pPreview->setPixmap(preview.scaled(preview.width()*7/10, preview.height()*7/10, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_pPreview->setPixmap(preview.scaled(std::min(preview.width(), std::max(preview.width()*7/10, m_pPreview->width())),
        std::min(preview.height(), std::max(preview.height()*7/10, m_pPreview->height())), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

int CUIPage::nextId() const
{
    return CSetupWizard::Page_Shell;
}

//////////////////////////////////////////////////////////////////////////////////////////
// CShellPage
// 

CShellPage::CShellPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle(tr("Configure <b>Sandboxie-Plus</b> shell integration"));
    setSubTitle(tr("Configure how Sandboxie-Plus should integrate with your system."));

    QVBoxLayout *layout = new QVBoxLayout;

    m_pAutoStart = new QCheckBox(tr("Start UI with Windows"));
    m_pAutoStart->setChecked(true);
    layout->addWidget(m_pAutoStart);
    registerField("isAutoStart", m_pAutoStart);

    m_pContecxtMenu = new QCheckBox(tr("Add 'Run Sandboxed' to the explorer context menu"));
    m_pContecxtMenu->setChecked(true);
    layout->addWidget(m_pContecxtMenu);
    registerField("useContecxtMenu", m_pContecxtMenu);

    m_pBrowserIcon = new QCheckBox(tr("Add desktop shortcut for starting Web browser under Sandboxie"));
    m_pBrowserIcon->setChecked(true);
    layout->addWidget(m_pBrowserIcon);
    registerField("useBrowserIcon", m_pBrowserIcon);

	m_pEditOnlyAdmin = new QCheckBox(tr("Only applications with admin rights can change configuration"));
	m_pEditOnlyAdmin->setChecked(false);
	connect(m_pEditOnlyAdmin,SIGNAL(clicked(bool)), this, SLOT(OnEditOnlyAdmin()));
	layout->addWidget(m_pEditOnlyAdmin);
	registerField("editAdminOnly", m_pEditOnlyAdmin);

    setLayout(layout);
}

void CShellPage::OnEditOnlyAdmin() 
{
    if (m_pEditOnlyAdmin->isChecked()) {
        if (QMessageBox::warning(this, tr("Warning"), tr("Enabling this option prevents changes to the Sandboxie.ini configuration from the user interface without admin rights. Be careful, as using Sandboxie Manager with normal user rights may result in a lockout. "
          "To make changes to the configuration, you must restart Sandboxie Manager as an admin by clicking 'Restart as Admin' in the 'Sandbox' menu in the main window."),
          QMessageBox::Ok, QMessageBox::Cancel) == QMessageBox::Cancel)
            m_pEditOnlyAdmin->setChecked(false);
    }
}

int CShellPage::nextId() const
{
    //return CSetupWizard::Page_WFP;
    return CSetupWizard::Page_Update;
    //return CSetupWizard::Page_Finish;
}

//////////////////////////////////////////////////////////////////////////////////////////
// CWFPPage
// 

/*CWFPPage::CWFPPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle(tr("Configure <b>Sandboxie-Plus</b> network filtering"));
    setSubTitle(tr("Sandboxie can use the Windows Filtering Platform (WFP) to restrict network access."));

    QVBoxLayout *layout = new QVBoxLayout;

    QLabel* pLabel = new QLabel;
    pLabel->setWordWrap(true);
    pLabel->setText(tr("Using WFP allows Sandboxie to reliably enforce IP/Port based rules for network access. "
        "Unlike system level application firewalls, Sandboxie can use different rules in each box for the same application. "
        "If you already have a good and reliable application firewall and do not need per box rules, you can leave this option unchecked. "
        "Without WFP enabled, Sandboxie will still be able to reliably and entirely block processes from accessing the network. "
        "However, this can cause the process to crash, as the driver blocks the required network device endpoints. "
        "Even with WFP disabled, Sandboxie offers to set IP/Port based rules, however these will be applied in user mode only and not be enforced by the driver. "
        "Hence, without WFP enabled, an intentionally malicious process could bypass those rules, but not the entire network block."));
    layout->addWidget(pLabel);

    m_pUseWFP = new QCheckBox(tr("Enable Windows Filtering Platform (WFP) support"));
    m_pUseWFP->setChecked(theAPI->GetGlobalSettings()->GetBool("NetworkEnableWFP", false));
    layout->addWidget(m_pUseWFP);
    registerField("useWFP", m_pUseWFP);

    setLayout(layout);
}

int CWFPPage::nextId() const
{
    return CSetupWizard::Page_Finish;
}*/

//////////////////////////////////////////////////////////////////////////////////////////
// CSBUpdate
// 

CSBUpdate::CSBUpdate(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle(tr("Configure <b>Sandboxie-Plus</b> online components"));
    setSubTitle(tr("Select which optional online components Sandboxie-Plus should keep up to date."));

    QGridLayout *layout = new QGridLayout;
    layout->setSpacing(3);

    int row = 0;
    int rows = 4;

    m_pIssues = new QCheckBox(tr("Get the latest Scripts for the Troubleshooting Wizard"));
    m_pIssues->setToolTip(tr("Check for latest troubleshooting scripts for the troubleshooting wizard."));
    layout->addWidget(m_pIssues, row++, 0, 1, rows);
    registerField("updateIssues", m_pIssues);

    m_pAddons = new QCheckBox(tr("Keep the list of optional Add-on components up to date"));
    m_pAddons->setToolTip(tr("Check for latest available add-ons."));
    layout->addWidget(m_pAddons, row++, 0, 1, rows);
    registerField("updateAddons", m_pAddons);

    layout->addItem(new QSpacerItem(10, 10, QSizePolicy::Fixed, QSizePolicy::Expanding), row++, 0);

    setLayout(layout);
}

void CSBUpdate::initializePage()
{
    m_pIssues->setChecked(true);
    m_pAddons->setChecked(true);
}

int CSBUpdate::nextId() const
{
    return CSetupWizard::Page_Finish;
}

//////////////////////////////////////////////////////////////////////////////////////////
// CFinishPage
// 

CFinishPage::CFinishPage(QWidget *parent)
    : QWizardPage(parent)
{
    setTitle(tr("Complete your configuration"));
    QPixmap Logo = QPixmap(theGUI->m_DarkTheme ? ":/SideLogoDM.png" : ":/SideLogo.png");
    int Scaling = theConf->GetInt("Options/FontScaling", 100);
    if(Scaling !=  100) Logo = Logo.scaled(Logo.width() * Scaling / 100, Logo.height() * Scaling / 100);
    setPixmap(QWizard::WatermarkPixmap, Logo);

    QVBoxLayout *layout = new QVBoxLayout;

    m_pLabel = new QLabel;
    m_pLabel->setWordWrap(true);
    m_pLabel->setText(tr("Almost complete, click Finish to apply all selected options and conclude the wizard."));
    layout->addWidget(m_pLabel);

    //QWidget* pSpacer = new QWidget();
    //pSpacer->setMinimumHeight(16);
    //layout->addWidget(pSpacer);
    
    //QLabel* pLabel = new QLabel;
    //pLabel->setWordWrap(true);
    //pLabel->setText(tr("Like with any other security product it's important to keep your Sandboxie-Plus up to date."));
    //layout->addWidget(pLabel);

    //m_pUpdate = new QCheckBox(tr("Keep Sandboxie-Plus up to date."));
    //m_pUpdate->setChecked(true);
    //layout->addWidget(m_pUpdate);
    //registerField("isUpdate", m_pUpdate);

    setLayout(layout);
}

int CFinishPage::nextId() const
{
    return -1;
}

void CFinishPage::initializePage()
{

}


//void ConclusionPage::setVisible(bool visible)
//{
//    QWizardPage::setVisible(visible);
//
//    if (visible) {
//        wizard()->setButtonText(QWizard::CustomButton1, tr("&Print"));
//        wizard()->setOption(QWizard::HaveCustomButton1, true);
//        connect(wizard(), &QWizard::customButtonClicked,
//                this, &ConclusionPage::printButtonClicked);
//    } else {
//        wizard()->setOption(QWizard::HaveCustomButton1, false);
//        disconnect(wizard(), &QWizard::customButtonClicked,
//                   this, &ConclusionPage::printButtonClicked);
//    }
//}
