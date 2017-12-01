/*  qgpgmequickjob.h

    Copyright (c) 2017 Intevation GmbH

    QGpgME is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.

    QGpgME is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    In addition, as a special exception, the copyright holders give
    permission to link the code of this program with any edition of
    the Qt library by Trolltech AS, Norway (or with modified versions
    of Qt that use the same license as Qt), and distribute linked
    combinations including the two.  You must obey the GNU General
    Public License in all respects for all of the code used other than
    Qt.  If you modify this file, you may extend this exception to
    your version of the file, but you are not obligated to do so.  If
    you do not wish to do so, delete this exception statement from
    your version.
*/
#ifndef QGPGME_QGPGMEQUICKJOB_H
#define QGPGME_QGPGMEQUICKJOB_H

#include "quickjob.h"

#include "threadedjobmixin.h"

namespace GpgME {
class Key;
}

class QDateTime;
class QString;

namespace QGpgME{

/**
 * Interface to the modern key manipulation functions.
 */
class QGpgMEQuickJob
#ifdef Q_MOC_RUN
    : public QuickJob
#else
    : public _detail::ThreadedJobMixin<QuickJob, std::tuple<GpgME::Error, QString, GpgME::Error> >
#endif
{
    Q_OBJECT
#ifdef Q_MOC_RUN
public Q_SLOTS:
    void slotFinished();
#endif
public:
    explicit QGpgMEQuickJob(GpgME::Context *context);
    ~QGpgMEQuickJob();

    void startCreate(const QString &uid,
                     const char *algo,
                     const QDateTime &expires = QDateTime(),
                     const GpgME::Key &key = GpgME::Key(),
                     unsigned int flags = 0) Q_DECL_OVERRIDE;
    void startAddUid(const GpgME::Key &key, const QString &uid) Q_DECL_OVERRIDE;
    void startRevUid(const GpgME::Key &key, const QString &uid) Q_DECL_OVERRIDE;
    void startAddSubkey(const GpgME::Key &key, const char *algo,
                        const QDateTime &expires = QDateTime(),
                        unsigned int flags = 0) Q_DECL_OVERRIDE;

Q_SIGNALS:
    void result(const GpgME::Error &error,
                const QString &auditLogAsHtml, const GpgME::Error &auditLogError);
};

}
#endif
