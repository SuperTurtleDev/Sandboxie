#include "stdafx.h"
#include "OnlineUpdater.h"
#include "../MiscHelpers/Common/Common.h"
#include "../MiscHelpers/Common/OtherFunctions.h"
#include "SandMan.h"
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include "Helpers/WinAdmin.h"
#include <windows.h>
#include <QRandomGenerator>

#ifdef QT_NO_SSL
#error Qt requires Open SSL support for the updater to work
#endif

COnlineUpdater::COnlineUpdater(QObject* parent) : QObject(parent)
{
	m_RequestManager = NULL;
}

void COnlineUpdater::StartJob(CUpdatesJob* pJob, const QUrl& Url)
{
	if (m_RequestManager == NULL)
		m_RequestManager = new CNetworkAccessManager(30 * 1000, this);

	QNetworkRequest Request = QNetworkRequest(Url);
	//Request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
	Request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	//Request.setRawHeader("Accept-Encoding", "gzip");
	QNetworkReply* pReply = m_RequestManager->get(Request);
	connect(pReply, SIGNAL(finished()), this, SLOT(OnRequestFinished()));
	connect(pReply, SIGNAL(downloadProgress(qint64, qint64)), pJob, SLOT(OnDownloadProgress(qint64, qint64)));

	connect(pJob->m_pProgress.data(), &CSbieProgress::Canceled, pReply, &QNetworkReply::abort);
	m_JobQueue.insert(pReply, pJob);
}

void COnlineUpdater::OnRequestFinished()
{
	QNetworkReply* pReply = qobject_cast<QNetworkReply*>(sender());
	CUpdatesJob* pJob = m_JobQueue.take(pReply);
	if (pJob) {
		pJob->Finish(pReply);
		pJob->deleteLater();
	}
	pReply->deleteLater();
}

quint64 COnlineUpdater::GetRandID()
{
	quint64 RandID = 0;
	theAPI->GetSecureParam("RandID", &RandID, sizeof(RandID));
	if (!RandID) {
		RandID = QRandomGenerator64::global()->generate();
		theAPI->SetSecureParam("RandID", &RandID, sizeof(RandID));
	}
	return RandID;
}

SB_PROGRESS COnlineUpdater::GetUpdates(QObject* receiver, const char* member, const QVariantMap& Params)
{
	QUrlQuery Query;
	Query.addQueryItem("action", "update");
	Query.addQueryItem("software", "sandboxie-plus");
#ifdef INSIDER_BUILD
	Query.addQueryItem("version", QString(__DATE__));
#else
	Query.addQueryItem("version", QString::number(VERSION_MJR) + "." + QString::number(VERSION_MIN) + "." + QString::number(VERSION_REV));
#endif
	Query.addQueryItem("system", "windows-" + QSysInfo::kernelVersion() + "-" + QSysInfo::currentCpuArchitecture());
	Query.addQueryItem("language", QLocale::system().name());
#ifdef _DEBUG
	Query.addQueryItem("debug", "1");
#endif

	quint64 RandID = COnlineUpdater::GetRandID();
	quint32 Hash = theAPI->GetUserSettings()->GetName().mid(13).toInt(NULL, 16);
	QString HashKey = QString::number(Hash, 16).rightJustified(8, '0').toUpper() + "-" + QString::number(RandID, 16).rightJustified(16, '0').toUpper();
	Query.addQueryItem("hash_key", HashKey);

	if (Params.contains("channel"))
		Query.addQueryItem("channel", Params["channel"].toString());
	else {
		QString ReleaseChannel = theConf->GetString("Options/ReleaseChannel", "stable");
		Query.addQueryItem("channel", ReleaseChannel);
	}

	Query.addQueryItem("auto", Params["manual"].toBool() ? "0" : "1");

#ifdef _DEBUG
	QString Test = Query.toString();
#endif

	QUrl Url("https://sandboxie-plus.com/update.php");
	Url.setQuery(Query);

	CUpdatesJob* pJob = new CGetUpdatesJob(Params, this);
	StartJob(pJob, Url);
	QObject::connect(pJob, SIGNAL(UpdateData(const QVariantMap&, const QVariantMap&)), receiver, member, Qt::QueuedConnection);
	return SB_PROGRESS(OP_ASYNC, pJob->m_pProgress);
}

void CGetUpdatesJob::Finish(QNetworkReply* pReply)
{
	QVariantMap Data;

	auto err = pReply->error();
	if (err != QNetworkReply::NoError)
	{
		//m_pProgress->Finish(SB_ERR(SB_OtherError, QVariantList() << tr("Updater Error: %1").arg(err), err));
		Data["error"] = true;
		Data["errorMsg"] = tr("%1").arg(err);
	}
	else
	{
		QByteArray Reply = pReply->readAll();

		Data = QJsonDocument::fromJson(Reply).toVariant().toMap();

		time_t CurrentDate = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
		theAPI->SetSecureParam("LastUpdate", &CurrentDate, sizeof(CurrentDate));
	}

	m_pProgress->Finish(SB_OK);

	emit UpdateData(Data, m_Params);
}

QDateTime COnlineUpdater::GetLastUpdateDate()
{
	time_t UpdateDate = 0;
	theAPI->GetSecureParam("LastUpdate", &UpdateDate, sizeof(UpdateDate));

	time_t CurrentDate = QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
	if (UpdateDate > CurrentDate) { // can't be in the future
		UpdateDate = 0;
		theAPI->SetSecureParam("LastUpdate", &UpdateDate, sizeof(UpdateDate));
	}

	return QDateTime::fromSecsSinceEpoch(UpdateDate);
}

SB_PROGRESS COnlineUpdater::DownloadFile(const QString& Url, QObject* receiver, const char* member, const QVariantMap& Params)
{
	CUpdatesJob* pJob = new CGetFileJob(Params, this);
	StartJob(pJob, Url);
	QObject::connect(pJob, SIGNAL(Download(const QString&, const QVariantMap&)), receiver, member, Qt::QueuedConnection);
	return SB_PROGRESS(OP_ASYNC, pJob->m_pProgress);
}

void CGetFileJob::Finish(QNetworkReply* pReply)
{
	m_pProgress->SetProgress(-1);

	if (m_pProgress->IsCanceled()) {
		m_pProgress->Finish(SB_OK);
		return;
	}

	const QString Url = pReply->request().url().toString();
	if (pReply->error() != QNetworkReply::NoError) {
		QString ErrorMessage = tr("Failed to download file from: %1").arg(Url);
		if (!pReply->errorString().isEmpty())
			ErrorMessage += "\n" + pReply->errorString();
		m_pProgress->Finish(SB_ERR(SB_OtherError, QVariantList() << ErrorMessage));
		return;
	}

	const qint64 Size = pReply->bytesAvailable();

	QString FilePath = m_Params["path"].toString();
	if (FilePath.isEmpty()) {
		QString Name = pReply->request().url().fileName();
		if (Name.isEmpty())
			Name = "unnamed_download.tmp";
		FilePath = ((COnlineUpdater*)parent())->GetUpdateDir(true) + "/" + Name;
	}

	QFile File(FilePath);
	if (!File.open(QFile::WriteOnly)) {
		const QString ErrorMessage = tr("Failed to download file from: %1").arg(Url)
			+ "\n" + File.errorString();
		m_pProgress->Finish(SB_ERR(SB_OtherError, QVariantList() << ErrorMessage));
		return;
	}

	qint64 Written = 0;
	bool WriteOk = true;
	while (pReply->bytesAvailable() > 0) {
		const QByteArray Chunk = pReply->read(4096);
		if (Chunk.isEmpty()) {
			WriteOk = false;
			break;
		}
		const qint64 Result = File.write(Chunk);
		if (Result != Chunk.size()) {
			WriteOk = false;
			break;
		}
		Written += Result;
	}

	if (!File.flush())
		WriteOk = false;

	const QDateTime Date = m_Params["setDate"].toDateTime();
	if (WriteOk && Date.isValid())
		File.setFileTime(Date, QFileDevice::FileModificationTime);

	const qint64 FileSize = File.size();
	File.close();

	if (!WriteOk || Written != Size || FileSize != Size) {
		QFile::remove(FilePath);
		m_pProgress->Finish(SB_ERR(SB_OtherError,
			QVariantList() << tr("Failed to download file from: %1").arg(Url)));
		return;
	}

	m_pProgress->Finish(SB_OK);
	emit Download(FilePath, m_Params);
}

SB_RESULT(int) COnlineUpdater::RunUpdater(const QStringList& Params, bool bSilent, bool Wait)
{
	if (bSilent) {
		SB_RESULT(int) Result = theAPI->RunUpdateUtility(Params, 2, Wait);
		if (!Result.IsError())
			return Result;
		// else fallback to ShellExecuteEx
		if (theConf->GetBool("Options/UpdateNoFallback", false))
			return Result;
	}

	std::wstring wFile = QString(QApplication::applicationDirPath() + "/UpdUtil.exe").replace("/", "\\").toStdWString();
	std::wstring wParams;
	foreach(const QString & Param, Params) {
		if (!wParams.empty()) wParams.push_back(L' ');
		wParams += L"\"" + Param.toStdWString() + L"\"";
	}

	int ExitCode = RunElevated(wFile, wParams, Wait ? INFINITE : 0);
	if (ExitCode == STATUS_PENDING && !Wait)
		ExitCode = 0;
	return CSbieResult<int>(ExitCode);
}

QString COnlineUpdater::GetUpdateDir(bool bCreate)
{
	QString TempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (TempDir.right(1) != "/")
		TempDir += "/";
	TempDir += "sandboxie-updater";
	// Note: must not end with a /
	if(bCreate)
		QDir().mkpath(TempDir);
	return TempDir;
}

QString COnlineUpdater::MakeVersionStr(const QVariantMap& Data)
{
	QString Str = Data["version"].toString();
	int iUpdate = Data["update"].toInt();
	if (iUpdate) Str += QChar('a' + (iUpdate - 1));
	return Str;
}

QString COnlineUpdater::ParseVersionStr(const QString& Str, int* pUpdate)
{
	int pos = Str.indexOf(QRegularExpression("[a-zA-Z]"));
	if (pos == -1)
		return Str;
	QString Ver = Str.left(pos);
	if (pUpdate) {
		QString Tmp = Str.mid(pos);
		*pUpdate = Tmp[0].toLatin1() - 'a' + 1;
	}
	return Ver;
}

QString COnlineUpdater::GetCurrentVersion()
{
	return QString::number(VERSION_MJR) + "." + QString::number(VERSION_MIN) + "." + QString::number(VERSION_REV);
}

int COnlineUpdater::GetCurrentUpdate()
{
	int iUpdate = 0;
	QString Version = ParseVersionStr(theConf->GetString("Updater/CurrentUpdate", 0), &iUpdate);
	if(Version != GetCurrentVersion() || iUpdate < VERSION_UPD)
		iUpdate = VERSION_UPD;
	return iUpdate;
}

quint32 COnlineUpdater::CurrentVersion()
{
	//quint8 myVersion[4] = { VERSION_UPD, VERSION_REV, VERSION_MIN, VERSION_MJR }; // ntohl
	quint8 myVersion[4] = { 0, VERSION_REV, VERSION_MIN, VERSION_MJR }; // ntohl
	quint32 MyVersion = *(quint32*)&myVersion;
	return MyVersion;
}

quint32 COnlineUpdater::VersionToInt(const QString& VersionStr)
{
	quint32 Version = 0;
	QStringList Nums = VersionStr.split(".");
	for (int i = 0, Bits = 24; i < Nums.count() && Bits >= 0; i++, Bits -= 8)
		Version |= (Nums[i].toInt() & 0xFF) << Bits;
	return Version;
}

bool COnlineUpdater::IsVersionNewer(const QString& VersionStr)
{
	if (VersionStr.isEmpty())
		return false;

#ifdef INSIDER_BUILD
	QString sVersion = VersionStr;
	if (sVersion[4] == ' ') sVersion[4] = '0';
	QDateTime VersionDate = QDateTime::fromString(sVersion, "MMM dd yyyy");

	sVersion = QString(__DATE__);
	if (sVersion[4] == ' ') sVersion[4] = '0';
	QDateTime BuildDate = QDateTime::fromString(sVersion, "MMM dd yyyy");

	return (VersionDate > BuildDate);
#else
	return VersionToInt(VersionStr) > CurrentVersion();
#endif
}
