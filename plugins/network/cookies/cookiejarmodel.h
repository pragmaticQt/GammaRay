/*
  cookiejarmodel.h

  This file is part of GammaRay, the Qt application inspection and
  manipulation tool.

  Copyright (C) 2016 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Volker Krause <volker.krause@kdab.com>

  Licensees holding valid commercial KDAB GammaRay licenses may use this file in
  accordance with GammaRay Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef GAMMARAY_COOKIEJARMODEL_H
#define GAMMARAY_COOKIEJARMODEL_H

#include <QAbstractTableModel>
#include <QList>
#include <QNetworkCookie>

QT_BEGIN_NAMESPACE
class QNetworkCookieJar;
QT_END_NAMESPACE

namespace GammaRay {

class CookieJarModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit CookieJarModel(QObject *parent = Q_NULLPTR);
    ~CookieJarModel();

    void setCookieJar(QNetworkCookieJar *cookieJar);

    int columnCount(const QModelIndex & parent) const Q_DECL_OVERRIDE;
    int rowCount(const QModelIndex & parent) const Q_DECL_OVERRIDE;
    QVariant data(const QModelIndex & index, int role) const Q_DECL_OVERRIDE;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const Q_DECL_OVERRIDE;

private:
    QNetworkCookieJar *m_cookieJar;
    QList<QNetworkCookie> m_cookies;
};
}

#endif // GAMMARAY_COOKIEJARMODEL_H