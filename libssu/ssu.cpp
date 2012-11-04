/**
 * @file ssu.cpp
 * @copyright 2012 Jolla Ltd.
 * @author Bernd Wachter <bernd.wachter@jollamobile.com>
 * @date 2012
 */

#include <QSystemDeviceInfo>

#include <QtXml/QDomDocument>
#include "ssu.h"
#include "../constants.h"

QTM_USE_NAMESPACE

Ssu::Ssu(): QObject(){
  errorFlag = false;

#ifdef SSUCONFHACK
  // dirty hack to make sure we can write to the configuration
  // this is currently required since there's no global gconf,
  // and we migth not yet have users on bootstrap
  QFileInfo settingsInfo(SSU_CONFIGURATION);
  if (settingsInfo.groupId() != SSU_GROUP_ID ||
      !settingsInfo.permission(QFile::WriteGroup)){
    QProcess proc;
    proc.start("/usr/bin/ssuconfperm");
    proc.waitForFinished();
  }
#endif

  settings = new QSettings(SSU_CONFIGURATION, QSettings::IniFormat);
  repoSettings = new QSettings(SSU_REPO_CONFIGURATION, QSettings::IniFormat);
  boardMappings = new QSettings(SSU_BOARD_MAPPING_CONFIGURATION, QSettings::IniFormat);
  QSettings defaultSettings(SSU_DEFAULT_CONFIGURATION, QSettings::IniFormat);

  int configVersion=0;
  int defaultConfigVersion=0;
  if (settings->contains("configVersion"))
    configVersion = settings->value("configVersion").toInt();
  if (defaultSettings.contains("configVersion"))
    defaultConfigVersion = defaultSettings.value("configVersion").toInt();

  if (configVersion < defaultConfigVersion){
    qDebug() << "Configuration is outdated, updating from " << configVersion
             << " to " << defaultConfigVersion;

    for (int i=configVersion+1;i<=defaultConfigVersion;i++){
      QStringList defaultKeys;
      QString currentSection = QString("%1/").arg(i);

      qDebug() << "Processing configuration version " << i;
      defaultSettings.beginGroup(currentSection);
      defaultKeys = defaultSettings.allKeys();
      defaultSettings.endGroup();
      foreach (const QString &key, defaultKeys){
        if (!settings->contains(key)){
          // Add new keys..
          settings->setValue(key, defaultSettings.value(currentSection + key));
          qDebug() << "Adding new key: " << key;
        } else {
          // ... or update the ones where default values has changed.
          QVariant oldValue;

          // check if an old value exists in an older configuration version
          for (int j=i-1;j>0;j--){
            if (defaultSettings.contains(QString("%1/").arg(j)+key)){
              oldValue = defaultSettings.value(QString("%1/").arg(j)+key);
              break;
            }
          }

          // skip updating if there is no old value, since we can't check if the
          // default value has changed
          if (oldValue.isNull())
            continue;

          QVariant newValue = defaultSettings.value(currentSection + key);
          if (oldValue == newValue){
            // old and new value match, no need to do anything, apart from beating the
            // person who added a useless key
            continue;
          } else {
            // default value has changed, so check if the configuration is still
            // using the old default value...
            QVariant currentValue = settings->value(key);
            // testcase: handles properly default update of thing with changed value in ssu.ini?
            if (currentValue == oldValue){
              // ...and update the key if it does
              settings->setValue(key, newValue);
              qDebug() << "Updating " << key << " from " << currentValue << " to " << newValue;
            }
          }
        }
      }
      settings->setValue("configVersion", i);
    }
  }

#ifdef TARGET_ARCH
  if (!settings->contains("arch"))
    settings->setValue("arch", TARGET_ARCH);
#else
// FIXME, try to guess a matching architecture
#warning "TARGET_ARCH not defined"
#endif
  settings->sync();

  manager = new QNetworkAccessManager(this);
  connect(manager, SIGNAL(finished(QNetworkReply *)),
          SLOT(requestFinished(QNetworkReply *)));
}

QPair<QString, QString> Ssu::credentials(QString scope){
  QPair<QString, QString> ret;
  settings->beginGroup("credentials-" + scope);
  ret.first = settings->value("username").toString();
  ret.second = settings->value("password").toString();
  settings->endGroup();
  return ret;
}

QString Ssu::credentialsScope(QString repoName, bool rndRepo){
  if (settings->contains("credentials-scope"))
    return settings->value("credentials-scope").toString();
  else
    return "your-configuration-is-broken-and-does-not-contain-credentials-scope";
}

QString Ssu::credentialsUrl(QString scope){
  if (settings->contains("credentials-url-" + scope))
    return settings->value("credentials-url-" + scope).toString();
  else
    return "your-configuration-is-broken-and-does-not-contain-credentials-url-for-" + scope;
}

QString Ssu::deviceFamily(){
  QString model = deviceModel();

  if (!cachedFamily.isEmpty())
    return cachedFamily;

  cachedFamily = "UNKNOWN";

  if (boardMappings->contains("variants/" + model))
    model = boardMappings->value("variants/" + model).toString();

  if (boardMappings->contains(model + "/family"))
    cachedFamily = boardMappings->value(model + "/family").toString();

  return cachedFamily;
}

QString Ssu::deviceModel(){
  QDir dir;
  QFile procCpuinfo;
  QStringList keys;

  if (!cachedModel.isEmpty())
    return cachedModel;

  boardMappings->beginGroup("file.exists");
  keys = boardMappings->allKeys();

  // check if the device can be identified by testing for a file
  foreach (const QString &key, keys){
    QString value = boardMappings->value(key).toString();
    if (dir.exists(value)){
      cachedModel = key;
      break;
    }
  }
  boardMappings->endGroup();
  if (!cachedModel.isEmpty()) return cachedModel;

  // check if the QSystemInfo model is useful
  QSystemDeviceInfo devInfo;
  QString model = devInfo.model();
  boardMappings->beginGroup("systeminfo.equals");
  keys = boardMappings->allKeys();
  foreach (const QString &key, keys){
    QString value = boardMappings->value(key).toString();
    if (model == value){
      cachedModel = key;
      break;
    }
  }
  boardMappings->endGroup();
  if (!cachedModel.isEmpty()) return cachedModel;

  // check if the device can be identified by a string in /proc/cpuinfo
  procCpuinfo.setFileName("/proc/cpuinfo");
  procCpuinfo.open(QIODevice::ReadOnly | QIODevice::Text);
  if (procCpuinfo.isOpen()){
    QTextStream in(&procCpuinfo);
    QString cpuinfo = in.readAll();
    boardMappings->beginGroup("cpuinfo.contains");
    keys = boardMappings->allKeys();

    foreach (const QString &key, keys){
      QString value = boardMappings->value(key).toString();
      if (cpuinfo.contains(value)){
        cachedModel = key;
        break;
      }
    }
    boardMappings->endGroup();
  }
  if (!cachedModel.isEmpty()) return cachedModel;


  // check if there's a match on arch ofr generic fallback. This probably
  // only makes sense for x86
  boardMappings->beginGroup("arch.equals");
  keys = boardMappings->allKeys();
  foreach (const QString &key, keys){
    QString value = boardMappings->value(key).toString();
    if (settings->value("arch").toString() == value){
      cachedModel = key;
      break;
    }
  }
  boardMappings->endGroup();
  if (cachedModel.isEmpty()) cachedModel = "UNKNOWN";

  return cachedModel;
}

QString Ssu::deviceUid(){
  QString IMEI;
  QSystemDeviceInfo devInfo;

  IMEI = devInfo.imei();
  // this might not be completely unique (or might change on reflash), but works for now
  if (IMEI == ""){
    if (deviceFamily() == "n950-n9" || deviceFamily() == "n900"){
      bool ok;
      QString IMEIenv = getenv("imei");
      IMEIenv.toLongLong(&ok, 10);
      if (ok && (IMEIenv.length() == 16 || IMEIenv.length() == 15))
        IMEI = IMEIenv;
    } else
      IMEI = devInfo.uniqueDeviceID();
  }
  return IMEI;
}

bool Ssu::error(){
  return errorFlag;
}

QString Ssu::flavour(){
  if (settings->contains("flavour"))
    return settings->value("flavour").toString();
  else
    return "release";
}

bool Ssu::isRegistered(){
  if (!settings->contains("privateKey"))
    return false;
  if (!settings->contains("certificate"))
    return false;
  return settings->value("registered").toBool();
}

QDateTime Ssu::lastCredentialsUpdate(){
  return settings->value("lastCredentialsUpdate").toDateTime();
}

QString Ssu::lastError(){
  return errorString;
}

bool Ssu::registerDevice(QDomDocument *response){
  QString certificateString = response->elementsByTagName("certificate").at(0).toElement().text();
  QSslCertificate certificate(certificateString.toAscii());

  if (certificate.isNull()){
    // make sure device is in unregistered state on failed registration
    settings->setValue("registered", false);
    setError("Certificate is invalid");
    return false;
  } else
    settings->setValue("certificate", certificate.toPem());

  QString privateKeyString = response->elementsByTagName("privateKey").at(0).toElement().text();
  QSslKey privateKey(privateKeyString.toAscii(), QSsl::Rsa);

  if (privateKey.isNull()){
    settings->setValue("registered", false);
    setError("Private key is invalid");
    return false;
  } else
    settings->setValue("privateKey", privateKey.toPem());

  // oldUser is just for reference purposes, in case we want to notify
  // about owner changes for the device
  QString oldUser = response->elementsByTagName("user").at(0).toElement().text();
  qDebug() << "Old user:" << oldUser;

  // if we came that far everything required for device registration is done
  settings->setValue("registered", true);
  settings->sync();
  emit registrationStatusChanged();
  return true;
}

QString Ssu::release(bool rnd){
  if (rnd)
    return settings->value("rndRelease").toString();
  else
    return settings->value("release").toString();
}

// RND repos have flavour (devel, testing, release), and release (latest, next)
// Release repos only have release (latest, next, version number)
QString Ssu::repoUrl(QString repoName, bool rndRepo, QHash<QString, QString> repoParameters){
  QString r;
  QStringList configSections;
  QStringList repoVariables;

  errorFlag = false;

  // fill in all arbitrary variables from ssu.ini
  settings->beginGroup("repository-url-variables");
  repoVariables = settings->allKeys();
  foreach (const QString &key, repoVariables){
    repoParameters.insert(key, settings->value(key).toString());
  }
  settings->endGroup();

  // add/overwrite some of the variables with sane ones
  if (rndRepo){
    repoParameters.insert("flavour", repoSettings->value(flavour()+"-flavour/flavour-pattern").toString());
    repoParameters.insert("release", settings->value("rndRelease").toString());
    configSections << flavour()+"-flavour" << "rnd" << "all";
  } else {
    repoParameters.insert("release", settings->value("release").toString());
    configSections << "release" << "all";
  }

  if (!repoParameters.contains("debugSplit"))
    repoParameters.insert("debugSplit", "packages");

  if (!repoParameters.contains("arch"))
    repoParameters.insert("arch", settings->value("arch").toString());

  repoParameters.insert("adaptation", settings->value("adaptation").toString());
  repoParameters.insert("deviceFamily", deviceFamily());
  repoParameters.insert("deviceModel", deviceModel());

  if (settings->contains("repository-urls/" + repoName))
    r = settings->value("repository-urls/" + repoName).toString();
  else {
    foreach (const QString &section, configSections){
      repoSettings->beginGroup(section);
      if (repoSettings->contains(repoName)){
        r = repoSettings->value(repoName).toString();
        repoSettings->endGroup();
        break;
      }
      repoSettings->endGroup();
    }
  }

  QHashIterator<QString, QString> i(repoParameters);
  while (i.hasNext()){
    i.next();
    r.replace(
      QString("%(%1)").arg(i.key()),
      i.value());
  }

  return r;
}

void Ssu::requestFinished(QNetworkReply *reply){
  QSslConfiguration sslConfiguration = reply->sslConfiguration();

  qDebug() << sslConfiguration.peerCertificate().issuerInfo(QSslCertificate::CommonName);
  qDebug() << sslConfiguration.peerCertificate().subjectInfo(QSslCertificate::CommonName);

  foreach (const QSslCertificate cert, sslConfiguration.peerCertificateChain()){
    qDebug() << "Cert from chain" << cert.subjectInfo(QSslCertificate::CommonName);
  }

  // what sucks more, this or goto?
  do {
    if (settings->contains("home-url")){
      QString homeUrl = settings->value("home-url").toString().arg("");
      homeUrl.remove(QRegExp("//+$"));
      QNetworkRequest request = reply->request();

      if (request.url().toString().startsWith(homeUrl, Qt::CaseInsensitive)){
        // we don't care about errors on download request
        if (reply->error() > 0) break;
        QByteArray data = reply->readAll();
        storeAuthorizedKeys(data);
        break;
      }
    }

    if (reply->error() > 0){
      pendingRequests--;
      setError(reply->errorString());
      return;
    } else {
      QByteArray data = reply->readAll();
      qDebug() << "RequestOutput" << data;

      QDomDocument doc;
      QString xmlError;
      if (!doc.setContent(data, &xmlError)){
        pendingRequests--;
        setError(tr("Unable to parse server response (%1)").arg(xmlError));
        return;
      }

      QString action = doc.elementsByTagName("action").at(0).toElement().text();

      if (!verifyResponse(&doc)) break;

      if (action == "register"){
        if (!registerDevice(&doc)) break;
      } else if (action == "credentials"){
        if (!setCredentials(&doc)) break;
      } else {
        pendingRequests--;
        setError(tr("Response to unknown action encountered: %1").arg(action));
        return;
      }
    }
  } while (false);

  pendingRequests--;
  if (pendingRequests == 0)
    emit done();
}

void Ssu::sendRegistration(QString username, QString password){
  errorFlag = false;

  QString ssuCaCertificate, ssuRegisterUrl;
  if (!settings->contains("ca-certificate")){
    setError("CA certificate for SSU not set (config key 'ca-certificate')");
    return;
  } else
    ssuCaCertificate = settings->value("ca-certificate").toString();

  if (!settings->contains("register-url")){
    setError("URL for SSU registration not set (config key 'register-url')");
    return;
  } else
    ssuRegisterUrl = settings->value("register-url").toString();

  QString IMEI = deviceUid();
  if (IMEI == ""){
    setError("No valid UID available for your device. For phones: is your modem online?");
    return;
  }

  QSslConfiguration sslConfiguration;
  if (!useSslVerify())
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);

  sslConfiguration.setCaCertificates(QSslCertificate::fromPath(ssuCaCertificate));

  QNetworkRequest request;
  request.setUrl(QUrl(QString(ssuRegisterUrl)
                      .arg(IMEI)
                   ));
  request.setSslConfiguration(sslConfiguration);
  request.setRawHeader("Authorization", "Basic " +
                       QByteArray(QString("%1:%2")
                                  .arg(username).arg(password)
                                  .toAscii()).toBase64());
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

  QUrl form;
  form.addQueryItem("protocolVersion", SSU_PROTOCOL_VERSION);
  form.addQueryItem("deviceModel", deviceModel());

  qDebug() << "Sending request to " << request.url();
  QNetworkReply *reply;

  pendingRequests++;
  reply = manager->post(request, form.encodedQuery());
  // we could expose downloadProgress() from reply in case we want progress info

  QString homeUrl = settings->value("home-url").toString().arg(username);
  if (!homeUrl.isEmpty()){
    // clear header, the other request bits are reusable
    request.setHeader(QNetworkRequest::ContentTypeHeader, 0);
    qDebug() << "sending request to " << homeUrl;
    request.setUrl(homeUrl + "/authorized_keys");
    pendingRequests++;
    manager->get(request);
  }
}

bool Ssu::setCredentials(QDomDocument *response){
  // generate list with all scopes for generic section, add sections
  QDomNodeList credentialsList = response->elementsByTagName("credentials");
  QStringList credentialScopes;
  for (int i=0;i<credentialsList.size();i++){
    QDomNode node = credentialsList.at(i);
    QString scope;

    QDomNamedNodeMap attributes = node.attributes();
    if (attributes.contains("scope")){
      scope = attributes.namedItem("scope").toAttr().value();
    } else {
      setError(tr("Credentials element does not have scope"));
      return false;
    }

    if (node.hasChildNodes()){
      QDomElement username = node.firstChildElement("username");
      QDomElement password = node.firstChildElement("password");
      if (username.isNull() || password.isNull()){
        setError(tr("Username and/or password not set"));
        return false;
      } else {
        settings->beginGroup("credentials-" + scope);
        settings->setValue("username", username.text());
        settings->setValue("password", password.text());
        settings->endGroup();
        settings->sync();
        credentialScopes.append(scope);
      }
    } else {
      setError("");
      return false;
    }
  }
  settings->setValue("credentialScopes", credentialScopes);
  settings->setValue("lastCredentialsUpdate", QDateTime::currentDateTime());
  settings->sync();
  emit credentialsChanged();

  return true;
}

void Ssu::setError(QString errorMessage){
  errorFlag = true;
  errorString = errorMessage;

  // assume that we don't even need to wait for other pending requests,
  // and just die. This is only relevant for CLI, which well exit after done()
  emit done();
}

void Ssu::setFlavour(QString flavour){
  settings->setValue("flavour", flavour);
  emit flavourChanged();
}

void Ssu::setRelease(QString release, bool rnd){
  if (rnd)
    settings->setValue("rndRelease", release);
  else
    settings->setValue("release", release);
}

void Ssu::storeAuthorizedKeys(QByteArray data){
  QDir dir;

  // only set the key for unprivileged users
  if (getuid() < 1000) return;

  if (dir.exists(dir.homePath() + "/.ssh/authorized_keys"))
    return;

  if (!dir.exists(dir.homePath() + "/.ssh"))
    if (!dir.mkdir(dir.homePath() + "/.ssh")) return;

  QFile::setPermissions(dir.homePath() + "/.ssh",
                        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

  QFile authorizedKeys(dir.homePath() + "/.ssh/authorized_keys");
  authorizedKeys.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
  authorizedKeys.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  QTextStream out(&authorizedKeys);
  out << data;
  out.flush();
  authorizedKeys.close();
}

void Ssu::updateCredentials(bool force){
  errorFlag = false;

  if (deviceUid() == ""){
    setError("No valid UID available for your device. For phones: is your modem online?");
    return;
  }

  QString ssuCaCertificate, ssuCredentialsUrl;
  if (!settings->contains("ca-certificate")){
    setError("CA certificate for SSU not set (config key 'ca-certificate')");
    return;
  } else
    ssuCaCertificate = settings->value("ca-certificate").toString();

  if (!settings->contains("credentials-url")){
    setError("URL for credentials update not set (config key 'credentials-url')");
    return;
  } else
    ssuCredentialsUrl = settings->value("credentials-url").toString();

  if (!isRegistered()){
    setError("Device is not registered.");
    return;
  }

  if (!force){
    // skip updating if the last update was less than a day ago
    QDateTime now = QDateTime::currentDateTime();

    if (settings->contains("lastCredentialsUpdate")){
      QDateTime last = settings->value("lastCredentialsUpdate").toDateTime();
      if (last >= now.addDays(-1)){
        emit done();
        return;
      }
    }
  }

  // check when the last update was, decide if an update is required
  QSslConfiguration sslConfiguration;
  if (!useSslVerify())
    sslConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);

  QSslKey privateKey(settings->value("privateKey").toByteArray(), QSsl::Rsa);
  QSslCertificate certificate(settings->value("certificate").toByteArray());

  QList<QSslCertificate> caCertificates;
  caCertificates << QSslCertificate::fromPath(ssuCaCertificate);
  sslConfiguration.setCaCertificates(caCertificates);

  sslConfiguration.setPrivateKey(privateKey);
  sslConfiguration.setLocalCertificate(certificate);

  QNetworkRequest request;
  request.setUrl(QUrl(ssuCredentialsUrl.arg(deviceUid())));

  qDebug() << request.url();
  request.setSslConfiguration(sslConfiguration);

  QUrl form;
  form.addQueryItem("protocolVersion", SSU_PROTOCOL_VERSION);

  pendingRequests++;
  manager->get(request);
}

bool Ssu::useSslVerify(){
  if (settings->contains("ssl-verify"))
    return settings->value("ssl-verify").toBool();
  else
    return true;
}

void Ssu::unregister(){
  settings->setValue("privateKey", "");
  settings->setValue("certificate", "");
  settings->setValue("registered", false);
  emit registrationStatusChanged();
}

bool Ssu::verifyResponse(QDomDocument *response){
  QString action = response->elementsByTagName("action").at(0).toElement().text();
  QString deviceId = response->elementsByTagName("deviceId").at(0).toElement().text();
  QString protocolVersion = response->elementsByTagName("protocolVersion").at(0).toElement().text();
  // compare device ids

  if (protocolVersion != SSU_PROTOCOL_VERSION){
    setError(
      tr("Response has unsupported protocol version %1, client requires version %2")
      .arg(protocolVersion)
      .arg(SSU_PROTOCOL_VERSION)
      );
    return false;
  }

  return true;
}
