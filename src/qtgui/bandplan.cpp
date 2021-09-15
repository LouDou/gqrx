/* -*- c++ -*- */
/*
 * Gqrx SDR: Software defined radio receiver powered by GNU Radio and Qt
 *           https://gqrx.dk/
 *
 * Copyright 2020 Dallas Epperson.
 *
 * Gqrx is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * Gqrx is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Gqrx; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include <algorithm>
#include <cmath>

#include <Qt>
#include <QDebug>
#include <QFile>
#include <QResource>
#include <QStringList>
#include <QTextStream>
#include <QString>
#include <QSet>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include "bandplan.h"

BandPlan* BandPlan::m_pThis = 0;

static inline QString textFrequency(const qint64 &freq) {
    const int mag = floor(log10(freq));
    if (mag >= 9) {
        return QString("%0 GHz").arg(freq / 1e9);
    }
    if (mag >= 6) {
        return QString("%0 MHz").arg(freq / 1e6);
    }
    if (mag >= 3) {
        return QString("%0 kHz").arg(freq / 1e3);
    }
    return QString("%0 kHz").arg(freq);
}

BandPlan::BandPlan()
{
    connect(this, SIGNAL(BandPlanParseError(QString)), this, SLOT(on_BandPlanParseError(QString)));
}

void BandPlan::create()
{
    m_pThis = new BandPlan;
}

BandPlan& BandPlan::Get()
{
    return *m_pThis;
}

void upgradeUserFile(const QString &cfg_dir, const QString &prevResourceName, const QString &bandplanFile)
{
    const QString fullPath = cfg_dir + "/" + bandplanFile;

    qInfo() << "BandPlan: File is " << fullPath;

    if (QFile::exists(fullPath))
    {
        QResource prevResource(":/textfiles/" + prevResourceName);
        auto prevTmp = cfg_dir + "/prev_tmp.csv";
        QFile::copy(prevResource.absoluteFilePath(), prevTmp);

        QFile prevFile(prevTmp);
        if (!prevFile.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            qInfo() << "Cannot open ref file";
            return;
        }
        auto prevData = prevFile.readAll();
        prevFile.close();

        QFile userFile(fullPath);
        if (!userFile.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            qInfo() << "Cannot open user bandplan file";
            return;
        }
        auto userData = userFile.readAll();
        userFile.close();

        if (prevData == userData) {
            qInfo() << "BandPlan: Found original file, will remove for upgrade";
            QFile::remove(fullPath);
        }

        QFile::remove(prevTmp);
    }
}

const QString installFile(const QString& cfg_dir, const QString &resourceName, const QString &targetName)
{
    const QString fullPath = cfg_dir + "/" + targetName;

    if (!QFile::exists(fullPath))
    {
        QResource resource(":/textfiles/" + resourceName);
        QFile::copy(resource.absoluteFilePath(), fullPath);
        QFile::setPermissions(fullPath, QFile::permissions(fullPath) | QFile::WriteOwner);
    }

    return fullPath;
}

void BandPlan::setConfigDir(const QString& cfg_dir)
{
    m_cfgPath = cfg_dir;

    upgradeUserFile(cfg_dir, "bandplan-v1.csv", "bandplan.csv");
    m_bandPlanFiles["user"] = installFile(cfg_dir, "bandplan-v2.csv", "bandplan.csv");
    qInfo() << "BandPlan: USER  File is " << m_bandPlanFiles["user"];

    m_bandPlanFiles["ofcom-data"] = installFile(cfg_dir, "ofcom-spectrum-map.json", "ofcom-spectrum-map.json");
    qInfo() << "BandPlan: OFCOM File is " << m_bandPlanFiles["ofcom-data"];
    m_bandPlanFiles["ofcom-map"] = installFile(cfg_dir, "ofcom-colour-map.json", "ofcom-colour-map.json");
}

void BandPlan::on_BandPlanParseError(QString filepath)
{
    QMessageBox msgBox;
    msgBox.setText(
                "The bandplan file on disk could not be completely read."
                );
    msgBox.setInformativeText(
                "Choose Restore Defaults to back up and replace your file with a new version.\n\n"
                "Press Cancel to leave your file intact.\n"
                "The bandplan will not show any items until you back up and remove your file from this location: \n\n" + filepath
                );
    msgBox.setStandardButtons(QMessageBox::Cancel | QMessageBox::RestoreDefaults);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();
    switch (ret) {
        case QMessageBox::Cancel:
            // Do nothing
            break;
        case QMessageBox::RestoreDefaults:
            QFile::rename(filepath, filepath + ".backup");
            setConfigDir(m_cfgPath);
            load();
            break;
        default:
            // should never be reached
            break;
    }
}

bool BandPlan::load()
{
    m_BandInfoLists.clear();

    // Load USER CSV
    {
        int goodlines = 0;
        int badlines = 0;

        QFile file(m_bandPlanFiles["user"]);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!file.atEnd())
            {
                QString line = QString::fromUtf8(file.readLine().trimmed());

                QStringList strings = line.split(",");
                if (line.isEmpty() || line.startsWith("#") || line.startsWith(",,") || strings.count() < 9) {
                    // qInfo() << "BandPlan: Ignoring Line:" << line;
                    badlines++;
                    continue;
                }

                BandInfo info;
                bool conv_ok = false;
                info.minFrequency = strings[0].toLongLong(&conv_ok);
                if (!conv_ok) {
                    badlines++;
                    continue;
                }
                info.maxFrequency = strings[1].toLongLong(&conv_ok);
                if (!conv_ok) {
                    badlines++;
                    continue;
                }
                info.modulation = strings[2].trimmed();
                if (info.modulation.length() == 0) {
                    badlines++;
                    continue;
                }
                info.step = strings[3].toInt(&conv_ok);
                if (!conv_ok) {
                    badlines++;
                    continue;
                }
                auto col = strings[4].trimmed();
                if (col.length() == 0) {
                    badlines++;
                    continue;
                }
                info.color = QColor(col).toHsl();
                if (info.color.alpha() == 255) {
                    info.color.setAlpha(0x90);
                }
                info.name = strings[5].trimmed();
                if (info.name.length() == 0) {
                    badlines++;
                    continue;
                }
                info.region = strings[6].trimmed();
                if (info.region.length() == 0) {
                    badlines++;
                    continue;
                }
                info.country = strings[7].trimmed();
                if (info.country.length() == 0) {
                    badlines++;
                    continue;
                }
                info.use = strings[8].trimmed();
                if (info.use.length() == 0) {
                    badlines++;
                    continue;
                }

                info.fullDescription = QString("%0 : %1 (%2 - %3)")
                    .arg(info.use)
                    .arg(info.name)
                    .arg(textFrequency(info.minFrequency))
                    .arg(textFrequency(info.maxFrequency));

                m_BandInfoLists["user"].append(info);

                m_filterValues.countries.insert(info.country);
                m_filterValues.modulations.insert(info.modulation);
                m_filterValues.regions.insert(info.region);
                m_filterValues.uses.insert(info.use);

                goodlines++;
            }
            file.close();
        }
        qInfo() << "BandPlan: Added" << m_BandInfoLists["user"].size() << "items from USER";

        if (badlines > goodlines || goodlines == 0)
        {
            qInfo() << "BandPlan: parse error; bad lines=" << badlines << " good lines=" << goodlines;
            emit BandPlanParseError(m_bandPlanFiles["user"]);
        }
    }

    // Load OFCOM JSON
    {
        QMap<QString, QString> ofcomSectorColors;
        QFile mapfile(m_bandPlanFiles["ofcom-map"]);
        if (mapfile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QJsonParseError error;
            QByteArray filedata = mapfile.readAll();
            QJsonObject doc = QJsonDocument::fromJson(filedata, &error).object();
            if (error.error == QJsonParseError::NoError) {
                const auto keys = doc.keys();
                for (const auto &key : keys) {
                    ofcomSectorColors[key] = doc[key].toString();
                }
            }
            mapfile.close();
        }

        QFile file(m_bandPlanFiles["ofcom-data"]);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QJsonParseError error;
            QByteArray filedata = file.readAll();
            QJsonObject doc = QJsonDocument::fromJson(filedata, &error).object();
            if (error.error == QJsonParseError::NoError) {
                qInfo() << "BandPlan: Read OFCOM Json dated " << doc["date_updated"].toString();

                m_BandInfoLists["ofcom"].clear();
                const auto bands = doc["bands"].toArray();
                for (const auto &bandItem : bands)
                {
                    const auto band = bandItem.toObject();
                    if (band["v"].toInt() != 1) {
                        continue;
                    }

                    BandInfo info;
                    info.minFrequency = band["lf"].toVariant().toLongLong();
                    info.maxFrequency = band["uf"].toVariant().toLongLong();
                    info.name         = band["u"].toString().trimmed();
                    info.use          = band["s"].toString().trimmed(); // sector
                    info.color        = ofcomSectorColors.contains(info.use)
                        ? QColor(ofcomSectorColors[info.use]).toHsl()
                        : QColor("olive").toHsl();
                    // quite transparent, because this dataset has lots of overlaps
                    info.color.setAlpha(0x30);

                    info.region = "Global";
                    info.country = "Global";
                    info.modulation = "Mixed";

                    info.fullDescription = QString("%0 : %1 (%2 - %3)")
                        .arg(info.use)
                        .arg(info.name)
                        .arg(textFrequency(info.minFrequency))
                        .arg(textFrequency(info.maxFrequency));

                    m_BandInfoLists["ofcom"].append(info);

                    m_filterValues.countries.insert(info.country);
                    m_filterValues.modulations.insert(info.modulation);
                    m_filterValues.regions.insert(info.region);
                    m_filterValues.uses.insert(info.use);
                }
            }
            else
            {
                qInfo() << "BandPlan: OFCOM Json parse error" << error.errorString() << "at" << error.offset;
            }
            file.close();
        }
        qInfo() << "BandPlan: Added" << m_BandInfoLists["ofcom"].size() << "items from OFCOM";
    }

    emit BandPlanChanged();

    return true;
}

QList<BandInfo> BandPlan::getBandsInRange(const QString &group, const BandInfoFilter &filter, qint64 low, qint64 high)
{
    QList<BandInfo> found;
    if (!m_BandInfoLists.contains(group)) {
        return found;
    }

    const auto bands = m_BandInfoLists[group];

    for (int i = 0; i < bands.size(); i++) {
        if (!filter.matches(bands[i])) continue;
        if (bands[i].maxFrequency < low) continue;
        if (bands[i].minFrequency > high) continue;
        found.append(bands[i]);
    }
    return found;
}

QList<BandInfo> BandPlan::getBandsEncompassing(const QString &group, const BandInfoFilter &filter, qint64 freq)
{
    QList<BandInfo> found;
    if (!m_BandInfoLists.contains(group)) {
        return found;
    }

    const auto bands = m_BandInfoLists[group];

    for (int i = 0; i < bands.size(); i++) {
        if (!filter.matches(m_BandInfoLists[group][i])) continue;
        if (bands[i].maxFrequency < freq) continue;
        if (bands[i].minFrequency > freq) continue;
        found.append(bands[i]);
    }
    return found;
}
