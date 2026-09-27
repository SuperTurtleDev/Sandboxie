#pragma once

#include <QObject>
#include "../MiscHelpers/Common/NetworkAccessManager.h"

#include "SbiePlusAPI.h"

class CUpdatesJob : public QObject
{
	Q_OBJECT

protected:
	friend class COnlineUpdater;

	CUpdatesJob(const QVariantMap& Params, QObject* parent = nullptr) : QObject(parent)
	{
		m_Params = Params;
		m_pProgress = CSbieProgressPtr(new CSbieProgress());
	}
	virtual ~CUpdatesJob() {}

	virtual void Finish(QNetworkReply* pReply) = 0;

	QVariantMap			m_Params;
	CSbieProgressPtr	m_pProgress;

private slots:
	void OnDownloadProgress(qint64 bytes, qint64 bytesTotal)
	{
		if (bytesTotal != 0 && !m_pProgress.isNull())
			m_pProgress->Progress(100 * bytes / bytesTotal);
	}
};

class CGetUpdatesJob : public CUpdatesJob
{
	Q_OBJECT

protected:
	friend class COnlineUpdater;

	CGetUpdatesJob(const QVariantMap& Params, QObject* parent = nullptr) : CUpdatesJob(Params, parent) {}

	virtual void Finish(QNetworkReply* pReply);

signals:
	void				UpdateData(const QVariantMap& Data, const QVariantMap& Params);
};

class CGetFileJob : public CUpdatesJob
{
	Q_OBJECT

protected:
	friend class COnlineUpdater;

	CGetFileJob(const QVariantMap& Params, QObject* parent = nullptr) : CUpdatesJob(Params, parent) {}

	virtual void Finish(QNetworkReply* pReply);

signals:
	void				Download(const QString& Path, const QVariantMap& Params);
};

class COnlineUpdater : public QObject
{
	Q_OBJECT
public:
	COnlineUpdater(QObject* parent);

	// generic online services, used for addons and helper scripts
	SB_PROGRESS			GetUpdates(QObject* receiver, const char* member, const QVariantMap& Params = QVariantMap());
	SB_PROGRESS			DownloadFile(const QString& Url, QObject* receiver, const char* member, const QVariantMap& Params = QVariantMap());

	static SB_RESULT(int) RunUpdater(const QStringList& Params, bool bSilent, bool Wait = false);

	static QString		MakeVersionStr(const QVariantMap& Data);
	static QString		ParseVersionStr(const QString& Str, int* pUpdate = NULL);
	static QString		GetCurrentVersion();
	static int			GetCurrentUpdate();
	static bool			IsVersionNewer(const QString& VersionStr);

	static QString		GetUpdateDir(bool bCreate = false);

	static quint32		CurrentVersion();
	static quint32		VersionToInt(const QString& VersionStr);

	static quint64		GetRandID();

	static QDateTime	GetLastUpdateDate();

private slots:
	void				OnRequestFinished();

protected:

	void				StartJob(CUpdatesJob* pJob, const QUrl& Url);

	CNetworkAccessManager*	m_RequestManager;
	QMap<QNetworkReply*, CUpdatesJob*> m_JobQueue;
};
