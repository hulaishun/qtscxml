/****************************************************************************
 **
 ** Copyright (c) 2015 Digia Plc
 ** For any questions to Digia, please use contact form at http://qt.digia.com/
 **
 ** All Rights Reserved.
 **
 ** NOTICE: All information contained herein is, and remains
 ** the property of Digia Plc and its suppliers,
 ** if any. The intellectual and technical concepts contained
 ** herein are proprietary to Digia Plc
 ** and its suppliers and may be covered by Finnish and Foreign Patents,
 ** patents in process, and are protected by trade secret or copyright law.
 ** Dissemination of this information or reproduction of this material
 ** is strictly forbidden unless prior written permission is obtained
 ** from Digia Plc.
 ****************************************************************************/

#include "scxmlstatemachine_p.h"
#include "executablecontent_p.h"
#include "scxmlevent_p.h"
#include "scxmlqstates_p.h"

#include <QAbstractState>
#include <QAbstractTransition>
#include <QFile>
#include <QHash>
#include <QJSEngine>
#include <QLoggingCategory>
#include <QState>
#include <QString>
#include <QTimer>

#include <QtCore/private/qstatemachine_p.h>

QT_BEGIN_NAMESPACE

namespace Scxml {
Q_LOGGING_CATEGORY(scxmlLog, "scxml.table")

namespace {
class InvokableScxml: public QScxmlInvokableService
{
public:
    InvokableScxml(StateMachine *stateMachine, const QString &id, const QVariantMap &data,
                   bool autoforward, QScxmlExecutableContent::ContainerId finalize, StateMachine *parent)
        : QScxmlInvokableService(id, data, autoforward, finalize, parent)
        , m_stateMachine(stateMachine)
    {
        stateMachine->setSessionId(id);
        stateMachine->setParentStateMachine(parent);
        stateMachine->init(data);
        stateMachine->start();
    }

    ~InvokableScxml()
    { delete m_stateMachine; }

    void submitEvent(QScxmlEvent *event) Q_DECL_OVERRIDE
    {
        m_stateMachine->submitEvent(event);
    }

private:
    StateMachine *m_stateMachine;
};
} // anonymous namespace

namespace Internal {
class WrappedQStateMachinePrivate: public QStateMachinePrivate
{
    Q_DECLARE_PUBLIC(WrappedQStateMachine)

public:
    WrappedQStateMachinePrivate(StateMachine *table)
        : m_table(table)
        , m_queuedEvents(Q_NULLPTR)
    {}
    ~WrappedQStateMachinePrivate()
    { delete m_queuedEvents; }

    int eventIdForDelayedEvent(const QByteArray &scxmlEventId);

protected: // overrides for QStateMachinePrivate:
#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)
    void noMicrostep() Q_DECL_OVERRIDE;
    void processedPendingEvents(bool didChange) Q_DECL_OVERRIDE;
    void beginMacrostep() Q_DECL_OVERRIDE;
    void endMacrostep(bool didChange) Q_DECL_OVERRIDE;

#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    void exitInterpreter() Q_DECL_OVERRIDE;
#endif // QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)

    void emitStateFinished(QState *forState, QFinalState *guiltyState) Q_DECL_OVERRIDE;
    void startupHook() Q_DECL_OVERRIDE;
#endif // QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)

public: // fields
    StateMachine *m_table;

    struct QueuedEvent
    {
        QueuedEvent(QEvent *event = Q_NULLPTR, QStateMachine::EventPriority priority = QStateMachine::NormalPriority)
            : event(event)
            , priority(priority)
        {}

        QEvent *event;
        QStateMachine::EventPriority priority;
    };
    QVector<QueuedEvent> *m_queuedEvents;
};

WrappedQStateMachine::WrappedQStateMachine(StateMachine *parent)
    : QStateMachine(*new WrappedQStateMachinePrivate(parent), parent)
{}

WrappedQStateMachine::WrappedQStateMachine(WrappedQStateMachinePrivate &dd, StateMachine *parent)
    : QStateMachine(dd, parent)
{}

StateMachine *WrappedQStateMachine::stateTable() const
{
    Q_D(const WrappedQStateMachine);

    return d->m_table;
}

StateMachinePrivate *WrappedQStateMachine::stateMachinePrivate()
{
    Q_D(WrappedQStateMachine);

    return StateMachinePrivate::get(d->m_table);
}
} // Internal namespace

class ScxmlError::ScxmlErrorPrivate
{
public:
    ScxmlErrorPrivate()
        : line(-1)
        , column(-1)
    {}

    QString fileName;
    int line;
    int column;
    QString description;
};

ScxmlError::ScxmlError()
    : d(Q_NULLPTR)
{}

ScxmlError::ScxmlError(const QString &fileName, int line, int column, const QString &description)
    : d(new ScxmlErrorPrivate)
{
    d->fileName = fileName;
    d->line = line;
    d->column = column;
    d->description = description;
}

ScxmlError::ScxmlError(const ScxmlError &other)
    : d(Q_NULLPTR)
{
    *this = other;
}

ScxmlError &ScxmlError::operator=(const ScxmlError &other)
{
    if (other.d) {
        if (!d)
            d = new ScxmlErrorPrivate;
        d->fileName = other.d->fileName;
        d->line = other.d->line;
        d->column = other.d->column;
        d->description = other.d->description;
    } else {
        delete d;
        d = Q_NULLPTR;
    }
    return *this;
}

ScxmlError::~ScxmlError()
{
    delete d;
    d = Q_NULLPTR;
}

bool ScxmlError::isValid() const
{
    return d != Q_NULLPTR;
}

QString ScxmlError::fileName() const
{
    return isValid() ? d->fileName : QString();
}

int ScxmlError::line() const
{
    return isValid() ? d->line : -1;
}

int ScxmlError::column() const
{
    return isValid() ? d->column : -1;
}

QString ScxmlError::description() const
{
    return isValid() ? d->description : QString();
}

QString ScxmlError::toString() const
{
    QString str;
    if (!isValid())
        return str;

    if (d->fileName.isEmpty())
        str = QStringLiteral("<Unknown File>");
    else
        str = d->fileName;
    if (d->line != -1) {
        str += QStringLiteral(":%1").arg(d->line);
        if (d->column != -1)
            str += QStringLiteral(":%1").arg(d->column);
    }
    str += QStringLiteral(": error: ") + d->description;

    return str;
}

QDebug operator<<(QDebug debug, const ScxmlError &error)
{
    debug << error.toString();
    return debug;
}

QDebug Q_SCXML_EXPORT operator<<(QDebug debug, const QVector<ScxmlError> &errors)
{
    foreach (const ScxmlError &error, errors) {
        debug << error << endl;
    }
    return debug;
}

TableData::~TableData()
{}

class QScxmlInvokableService::Data
{
public:
    Data(const QString &id, const QVariantMap &data, bool autoforward,
         QScxmlExecutableContent::ContainerId finalize, StateMachine *parent)
        : id(id)
        , data(data)
        , autoforward(autoforward)
        , parent(parent)
        , finalize(finalize)
    {}

    QString id;
    QVariantMap data;
    bool autoforward;
    StateMachine *parent;
    QScxmlExecutableContent::ContainerId finalize;
};

QScxmlInvokableService::QScxmlInvokableService(const QString &id,
                                             const QVariantMap &data,
                                             bool autoforward,
                                             QScxmlExecutableContent::ContainerId finalize,
                                             StateMachine *parent)
    : d(new Data(id, data, autoforward, finalize, parent))
{}

QScxmlInvokableService::~QScxmlInvokableService()
{
    delete d;
}

QString QScxmlInvokableService::id() const
{
    return d->id;
}

bool QScxmlInvokableService::autoforward() const
{
    return d->autoforward;
}

QVariantMap QScxmlInvokableService::data() const
{
    return d->data;
}

StateMachine *QScxmlInvokableService::parent() const
{
    return d->parent;
}

void QScxmlInvokableService::finalize()
{
    auto smp = StateMachinePrivate::get(parent());
    smp->m_executionEngine->execute(d->finalize);
}

class QScxmlInvokableServiceFactory::Data
{
public:
    Data(QScxmlExecutableContent::StringId invokeLocation, QScxmlExecutableContent::StringId id,
         QScxmlExecutableContent::StringId idPrefix, QScxmlExecutableContent::StringId idlocation,
         const QVector<QScxmlExecutableContent::StringId> &namelist, bool autoforward,
         const QVector<QScxmlInvokableServiceFactory::Param> &params,
         QScxmlExecutableContent::ContainerId finalize)
        : invokeLocation(invokeLocation)
        , id(id)
        , idPrefix(idPrefix)
        , idlocation(idlocation)
        , namelist(namelist)
        , autoforward(autoforward)
        , params(params)
        , finalize(finalize)
    {}

    QScxmlExecutableContent::StringId invokeLocation;
    QScxmlExecutableContent::StringId id;
    QScxmlExecutableContent::StringId idPrefix;
    QScxmlExecutableContent::StringId idlocation;
    QVector<QScxmlExecutableContent::StringId> namelist;
    bool autoforward;
    QVector<QScxmlInvokableServiceFactory::Param> params;
    QScxmlExecutableContent::ContainerId finalize;

};

QScxmlInvokableServiceFactory::QScxmlInvokableServiceFactory(
        QScxmlExecutableContent::StringId invokeLocation,  QScxmlExecutableContent::StringId id,
        QScxmlExecutableContent::StringId idPrefix, QScxmlExecutableContent::StringId idlocation,
        const QVector<QScxmlExecutableContent::StringId> &namelist, bool autoforward,
        const QVector<Param> &params, QScxmlExecutableContent::ContainerId finalize)
    : d(new Data(invokeLocation, id, idPrefix, idlocation, namelist, autoforward, params, finalize))
{}

QScxmlInvokableServiceFactory::~QScxmlInvokableServiceFactory()
{
    delete d;
}

QString QScxmlInvokableServiceFactory::calculateId(StateMachine *parent, bool *ok) const
{
    Q_ASSERT(ok);
    *ok = true;
    auto table = parent->tableData();

    if (d->id != QScxmlExecutableContent::NoString) {
        return table->string(d->id);
    }

    QString id = StateMachine::generateSessionId(table->string(d->idPrefix));

    if (d->idlocation != QScxmlExecutableContent::NoString) {
        auto idloc = table->string(d->idlocation);
        auto ctxt = table->string(d->invokeLocation);
        parent->dataModel()->setProperty(idloc, id, ctxt, ok);
        if (!*ok)
            return QString();
    }

    return id;
}

QVariantMap QScxmlInvokableServiceFactory::calculateData(StateMachine *parent, bool *ok) const
{
    Q_ASSERT(ok);

    QVariantMap result;
    auto dataModel = parent->dataModel();
    auto tableData = parent->tableData();

    foreach (const Param &param, d->params) {
        auto name = tableData->string(param.name);

        if (param.expr != NoEvaluator) {
            *ok = false;
            auto v = dataModel->evaluateToVariant(param.expr, ok);
            if (!*ok)
                return QVariantMap();
            result.insert(name, v);
        } else {
            QString loc;
            if (param.location != QScxmlExecutableContent::NoString) {
                loc = tableData->string(param.location);
            }

            if (loc.isEmpty()) {
                // TODO: error message?
                *ok = false;
                return QVariantMap();
            }

            auto v = dataModel->property(loc);
            result.insert(name, v);
        }
    }

    foreach (QScxmlExecutableContent::StringId locid, d->namelist) {
        QString loc;
        if (locid != QScxmlExecutableContent::NoString) {
            loc = tableData->string(locid);
        }
        if (loc.isEmpty()) {
            // TODO: error message?
            *ok = false;
            return QVariantMap();
        }
        auto v = dataModel->property(loc);
        result.insert(loc, v);
    }

    return result;
}

bool QScxmlInvokableServiceFactory::autoforward() const
{
    return d->autoforward;
}

QScxmlExecutableContent::ContainerId QScxmlInvokableServiceFactory::finalizeContent() const
{
    return d->finalize;
}

ScxmlEventFilter::~ScxmlEventFilter()
{}

QScxmlInvokableScxmlServiceFactory::QScxmlInvokableScxmlServiceFactory(
        QScxmlExecutableContent::StringId invokeLocation,
        QScxmlExecutableContent::StringId id,
        QScxmlExecutableContent::StringId idPrefix,
        QScxmlExecutableContent::StringId idlocation,
        const QVector<QScxmlExecutableContent::StringId> &namelist,
        bool doAutoforward,
        const QVector<QScxmlInvokableServiceFactory::Param> &params,
        QScxmlExecutableContent::ContainerId finalize)
    : QScxmlInvokableServiceFactory(invokeLocation, id, idPrefix, idlocation, namelist,
                                   doAutoforward, params, finalize)
{}

QScxmlInvokableService *QScxmlInvokableScxmlServiceFactory::finishInvoke(StateMachine *child, StateMachine *parent)
{
    bool ok = false;
    auto id = calculateId(parent, &ok);
    if (!ok)
        return Q_NULLPTR;
    auto data = calculateData(parent, &ok);
    if (!ok)
        return Q_NULLPTR;
    child->setIsInvoked(true);
    return new InvokableScxml(child, id, data, autoforward(), finalizeContent(), parent);
}

QAtomicInt StateMachinePrivate::m_sessionIdCounter = QAtomicInt(0);

StateMachinePrivate::StateMachinePrivate()
    : QObjectPrivate()
    , m_sessionId(StateMachine::generateSessionId(QStringLiteral("session-")))
    , m_isInvoked(false)
    , m_dataModel(Q_NULLPTR)
    , m_dataBinding(StateMachine::EarlyBinding)
    , m_executionEngine(Q_NULLPTR)
    , m_tableData(Q_NULLPTR)
    , m_qStateMachine(Q_NULLPTR)
    , m_eventFilter(Q_NULLPTR)
    , m_parentStateMachine(Q_NULLPTR)
{}

StateMachinePrivate::~StateMachinePrivate()
{
    delete m_executionEngine;
}

void StateMachinePrivate::setQStateMachine(Internal::WrappedQStateMachine *stateMachine)
{
    m_qStateMachine = stateMachine;
}

static QScxmlState *findState(const QString &scxmlName, QStateMachine *parent)
{
    QList<QObject *> worklist;
    worklist.reserve(parent->children().size() + parent->configuration().size());
    worklist.append(parent);

    while (!worklist.isEmpty()) {
        QObject *obj = worklist.takeLast();
        if (QScxmlState *state = qobject_cast<QScxmlState *>(obj)) {
            if (state->objectName() == scxmlName)
                return state;
        }
        worklist.append(obj->children());
    }

    return Q_NULLPTR;
}

QAbstractState *StateMachinePrivate::stateByScxmlName(const QString &scxmlName)
{
    return findState(scxmlName, m_qStateMachine);
}

StateMachinePrivate::ParserData *StateMachinePrivate::parserData()
{
    if (m_parserData.isNull())
        m_parserData.reset(new ParserData);
    return m_parserData.data();
}

StateMachine *StateMachine::fromFile(const QString &fileName, QScxmlDataModel *dataModel)
{
    QFile scxmlFile(fileName);
    if (!scxmlFile.open(QIODevice::ReadOnly)) {
        QVector<ScxmlError> errors({
                                ScxmlError(scxmlFile.fileName(), 0, 0, QStringLiteral("cannot open for reading"))
                            });
        auto table = new StateMachine;
        StateMachinePrivate::get(table)->parserData()->m_errors = errors;
        return table;
    }

    StateMachine *table = fromData(&scxmlFile, fileName, dataModel);
    scxmlFile.close();
    return table;
}

StateMachine *StateMachine::fromData(QIODevice *data, const QString &fileName, QScxmlDataModel *dataModel)
{
    QXmlStreamReader xmlReader(data);
    Scxml::QScxmlParser parser(&xmlReader);
    parser.setFileName(fileName);
    parser.parse();
    auto table = parser.instantiateStateMachine();
    if (dataModel) {
        table->setDataModel(dataModel);
    } else {
        parser.instantiateDataModel(table);
    }
    return table;
}

QVector<ScxmlError> StateMachine::errors() const
{
    Q_D(const StateMachine);
    return d->m_parserData ? d->m_parserData->m_errors : QVector<ScxmlError>();
}

StateMachine::StateMachine(QObject *parent)
    : QObject(*new StateMachinePrivate, parent)
{
    Q_D(StateMachine);
    d->m_executionEngine = new QScxmlExecutableContent::ExecutionEngine(this);
    d->setQStateMachine(new Internal::WrappedQStateMachine(this));
    connect(d->m_qStateMachine, &QStateMachine::finished, this, &StateMachine::onFinished);
    connect(d->m_qStateMachine, &QStateMachine::finished, this, &StateMachine::finished);
}

StateMachine::StateMachine(StateMachinePrivate &dd, QObject *parent)
    : QObject(dd, parent)
{
    Q_D(StateMachine);
    d->m_executionEngine = new QScxmlExecutableContent::ExecutionEngine(this);
    connect(d->m_qStateMachine, &QStateMachine::finished, this, &StateMachine::onFinished);
    connect(d->m_qStateMachine, &QStateMachine::finished, this, &StateMachine::finished);
}

QString StateMachine::sessionId() const
{
    Q_D(const StateMachine);

    return d->m_sessionId;
}

void StateMachine::setSessionId(const QString &id)
{
    Q_D(StateMachine);
    d->m_sessionId = id;
}

QString StateMachine::generateSessionId(const QString &prefix)
{
    int id = ++StateMachinePrivate::m_sessionIdCounter;
    return prefix + QString::number(id);
}

bool StateMachine::isInvoked() const
{
    Q_D(const StateMachine);
    return d->m_isInvoked;
}

void StateMachine::setIsInvoked(bool invoked)
{
    Q_D(StateMachine);
    d->m_isInvoked = invoked;
}

QScxmlDataModel *StateMachine::dataModel() const
{
    Q_D(const StateMachine);

    return d->m_dataModel;
}

void StateMachine::setDataModel(QScxmlDataModel *dataModel)
{
    Q_D(StateMachine);

    d->m_dataModel = dataModel;
    dataModel->setTable(this);
}

void StateMachine::setDataBinding(StateMachine::BindingMethod b)
{
    Q_D(StateMachine);

    d->m_dataBinding = b;
}

StateMachine::BindingMethod StateMachine::dataBinding() const
{
    Q_D(const StateMachine);

    return d->m_dataBinding;
}

TableData *StateMachine::tableData() const
{
    Q_D(const StateMachine);

    return d->m_tableData;
}

void StateMachine::setTableData(TableData *tableData)
{
    Q_D(StateMachine);

    d->m_tableData = tableData;
}

void StateMachine::doLog(const QString &label, const QString &msg)
{
    qCDebug(scxmlLog) << label << ":" << msg;
    emit log(label, msg);
}

StateMachine *StateMachine::parentStateMachine() const
{
    Q_D(const StateMachine);
    return d->m_parentStateMachine;
}

void StateMachine::setParentStateMachine(StateMachine *parent)
{
    Q_D(StateMachine);
    d->m_parentStateMachine = parent;
}

void Internal::WrappedQStateMachine::beginSelectTransitions(QEvent *event)
{
    Q_D(WrappedQStateMachine);

    if (event && event->type() == QScxmlEvent::scxmlEventType) {
        stateMachinePrivate()->m_event = *static_cast<QScxmlEvent *>(event);
        d->m_table->dataModel()->setEvent(stateMachinePrivate()->m_event);

        auto scxmlEvent = static_cast<QScxmlEvent *>(event);
        auto smp = stateMachinePrivate();

        foreach (QScxmlInvokableService *service, smp->m_registeredServices) {
            if (scxmlEvent->invokeId() == service->id()) {
                service->finalize();
            }
            if (service->autoforward()) {
                qCDebug(scxmlLog) << "auto-forwarding event" << scxmlEvent->name()
                                  << "from" << stateTable()->name() << "to service" << service->id();
                service->submitEvent(new QScxmlEvent(*scxmlEvent));
            }
        }

        auto e = static_cast<QScxmlEvent *>(event);
        if (smp->m_eventFilter && !smp->m_eventFilter->handle(e, d->m_table)) {
            e->makeIgnorable();
            e->clear();
            smp->m_event.clear();
            return;
        }
    } else {
        stateMachinePrivate()->m_event.clear();
        d->m_table->dataModel()->setEvent(stateMachinePrivate()->m_event);
    }
}

void Internal::WrappedQStateMachine::beginMicrostep(QEvent *event)
{
    Q_D(WrappedQStateMachine);

    qCDebug(scxmlLog) << d->m_table->tableData()->name() << " started microstep from state" << d->m_table->activeStates()
                      << "with event" << stateMachinePrivate()->m_event.name() << "from event type" << event->type();
}

void Internal::WrappedQStateMachine::endMicrostep(QEvent *event)
{
    Q_D(WrappedQStateMachine);
    Q_UNUSED(event);

    qCDebug(scxmlLog) << d->m_table->tableData()->name() << " finished microstep in state (" << d->m_table->activeStates() << ")";
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)

void Internal::WrappedQStateMachinePrivate::noMicrostep()
{
    qCDebug(scxmlLog) << m_table->tableData()->name() << " had no transition, stays in state (" << m_table->activeStates() << ")";
}

void Internal::WrappedQStateMachinePrivate::processedPendingEvents(bool didChange)
{
    qCDebug(scxmlLog) << m_table->tableData()->name() << " finishedPendingEvents " << didChange << " in state ("
                      << m_table->activeStates() << ")";
    emit m_table->reachedStableState(didChange);
}

void Internal::WrappedQStateMachinePrivate::beginMacrostep()
{
}

void Internal::WrappedQStateMachinePrivate::endMacrostep(bool didChange)
{
    qCDebug(scxmlLog) << m_table->tableData()->name() << " endMacrostep " << didChange << " in state ("
                      << m_table->activeStates() << ")";
}

#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
void Internal::WrappedQStateMachinePrivate::exitInterpreter()
{
    Q_Q(WrappedQStateMachine);

    foreach (QAbstractState *s, configuration) {
        QScxmlExecutableContent::ContainerId onExitInstructions = QScxmlExecutableContent::NoInstruction;
        if (QScxmlFinalState *finalState = qobject_cast<QScxmlFinalState *>(s)) {
            StateMachinePrivate::get(m_table)->m_executionEngine->execute(finalState->doneData(), QVariant());
            onExitInstructions = QScxmlFinalState::Data::get(finalState)->onExitInstructions;
        } else if (QScxmlState *state = qobject_cast<QScxmlState *>(s)) {
            onExitInstructions = QScxmlState::Data::get(state)->onExitInstructions;
        }

        if (onExitInstructions != QScxmlExecutableContent::NoInstruction)
            StateMachinePrivate::get(m_table)->m_executionEngine->execute(onExitInstructions);

        if (QScxmlFinalState *finalState = qobject_cast<QScxmlFinalState *>(s)) {
            if (finalState->parent() == q) {
                auto psm = m_table->parentStateMachine();
                if (psm) {
                    auto done = new QScxmlEvent;
                    done->setName(QByteArray("done.invoke.") + m_table->sessionId().toUtf8());
                    done->setInvokeId(m_table->sessionId());
                    qCDebug(scxmlLog) << "submitting event" << done->name() << "to" << psm->name();
                    psm->submitEvent(done);
                }
            }
        }
    }
}
#endif // QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)

void Internal::WrappedQStateMachinePrivate::emitStateFinished(QState *forState, QFinalState *guiltyState)
{
    Q_Q(WrappedQStateMachine);

    if (QScxmlFinalState *finalState = qobject_cast<QScxmlFinalState *>(guiltyState)) {
        if (!q->isRunning())
            return;
        StateMachinePrivate::get(m_table)->m_executionEngine->execute(finalState->doneData(), forState->objectName());
    }

    QStateMachinePrivate::emitStateFinished(forState, guiltyState);
}

void Internal::WrappedQStateMachinePrivate::startupHook()
{
    Q_Q(WrappedQStateMachine);

    q->submitQueuedEvents();
}

#endif // QT_VERSION >= QT_VERSION_CHECK(5, 5, 0)

int Internal::WrappedQStateMachinePrivate::eventIdForDelayedEvent(const QByteArray &scxmlEventId)
{
    QMutexLocker locker(&delayedEventsMutex);

    QHash<int, DelayedEvent>::const_iterator it;
    for (it = delayedEvents.constBegin(); it != delayedEvents.constEnd(); ++it) {
        if (QScxmlEvent *e = dynamic_cast<QScxmlEvent *>(it->event)) {
            if (e->sendId() == scxmlEventId) {
                return it.key();
            }
        }
    }

    return -1;
}

QStringList StateMachine::activeStates(bool compress)
{
    Q_D(StateMachine);

    QSet<QAbstractState *> config = QStateMachinePrivate::get(d->m_qStateMachine)->configuration;
    if (compress)
        foreach (const QAbstractState *s, config)
            config.remove(s->parentState());
    QStringList res;
    foreach (const QAbstractState *s, config) {
        QString id = s->objectName();
        if (!id.isEmpty()) {
            res.append(id);
        }
    }
    std::sort(res.begin(), res.end());
    return res;
}

bool StateMachine::isActive(const QString &scxmlStateName) const
{
    Q_D(const StateMachine);
    QSet<QAbstractState *> config = QStateMachinePrivate::get(d->m_qStateMachine)->configuration;
    foreach (QAbstractState *s, config) {
        if (s->objectName() == scxmlStateName) {
            return true;
        }
    }
    return false;
}

bool StateMachine::hasState(const QString &scxmlStateName) const
{
    Q_D(const StateMachine);
    return findState(scxmlStateName, d->m_qStateMachine) != Q_NULLPTR;
}

QMetaObject::Connection StateMachine::connect(const QString &scxmlStateName, const char *signal,
                                            const QObject *receiver, const char *method,
                                            Qt::ConnectionType type)
{
    Q_D(StateMachine);
    QScxmlState *state = findState(scxmlStateName, d->m_qStateMachine);
    return QObject::connect(state, signal, receiver, method, type);
}

ScxmlEventFilter *StateMachine::scxmlEventFilter() const
{
    Q_D(const StateMachine);
    return d->m_eventFilter;
}

void StateMachine::setScxmlEventFilter(ScxmlEventFilter *newFilter)
{
    Q_D(StateMachine);
    d->m_eventFilter = newFilter;
}

void StateMachine::executeInitialSetup()
{
    Q_D(const StateMachine);
    d->m_executionEngine->execute(tableData()->initialSetup());
}

static bool loopOnSubStates(QState *startState,
                            std::function<bool(QState *)> enteringState = Q_NULLPTR,
                            std::function<void(QState *)> exitingState = Q_NULLPTR,
                            std::function<void(QAbstractState *)> inAbstractState = Q_NULLPTR)
{
    QList<int> pos;
    QState *parentAtt = startState;
    QObjectList childs = startState->children();
    pos << 0;
    while (!pos.isEmpty()) {
        bool goingDeeper = false;
        for (int i = pos.last(); i < childs.size() ; ++i) {
            if (QAbstractState *as = qobject_cast<QAbstractState *>(childs.at(i))) {
                if (QState *s = qobject_cast<QState *>(as)) {
                    if (enteringState && !enteringState(s))
                        continue;
                    pos.last() = i + 1;
                    parentAtt = s;
                    childs = s->children();
                    pos << 0;
                    goingDeeper = !childs.isEmpty();
                    break;
                } else if (inAbstractState) {
                    inAbstractState(as);
                }
            }
        }
        if (!goingDeeper) {
            do {
                pos.removeLast();
                if (pos.isEmpty())
                    break;
                if (exitingState)
                    exitingState(parentAtt);
                parentAtt = parentAtt->parentState();
                childs = parentAtt->children();
            } while (!pos.isEmpty() && pos.last() >= childs.size());
        }
    }
    return true;
}

bool StateMachine::init(const QVariantMap &initialDataValues)
{
    Q_D(StateMachine);

    dataModel()->setup(initialDataValues);
    executeInitialSetup();

    bool res = true;
    loopOnSubStates(d->m_qStateMachine, std::function<bool(QState *)>(), [&res](QState *state) {
        if (QScxmlState *s = qobject_cast<QScxmlState *>(state))
            if (!s->init())
                res = false;
        if (QScxmlFinalState *s = qobject_cast<QScxmlFinalState *>(state))
            if (!s->init())
                res = false;
        foreach (QAbstractTransition *t, state->transitions()) {
            if (QScxmlTransition *scTransition = qobject_cast<QScxmlTransition *>(t))
                if (!scTransition->init())
                    res = false;
        }
    });
    foreach (QAbstractTransition *t, d->m_qStateMachine->transitions()) {
        if (QScxmlTransition *scTransition = qobject_cast<QScxmlTransition *>(t))
            if (!scTransition->init())
                res = false;
    }
    return res;
}

bool StateMachine::isRunning() const
{
    Q_D(const StateMachine);

    return d->m_qStateMachine->isRunning();
}

QString StateMachine::name() const
{
    return tableData()->name();
}

void StateMachine::submitError(const QByteArray &type, const QString &msg, const QByteArray &sendid)
{
    qCDebug(scxmlLog) << "machine" << name() << "had error" << type << ":" << msg;
    submitEvent(EventBuilder::errorEvent(this, type, sendid));
}

void StateMachine::routeEvent(QScxmlEvent *e)
{
    if (!e)
        return;

    if (e->origin() == QStringLiteral("#_parent")) {
        if (auto psm = parentStateMachine()) {
            qCDebug(scxmlLog) << "routing event" << e->name() << "from" << name() << "to parent" << psm->name();
            psm->submitEvent(e);
        } else {
            qCDebug(scxmlLog) << "machine" << name() << "is not invoked, so it cannot route a message to #_parent";
            delete e;
        }
    } else {
        // FIXME: event routing to child services
        submitEvent(e);
    }
}

void StateMachine::submitEvent(QScxmlEvent *e)
{
    Q_D(StateMachine);

    if (!e)
        return;

    if (e->delay() > 0) {
        qCDebug(scxmlLog) << name() << ": submitting event" << e->name() << "with delay" << e->delay() << "ms" << "and sendid" << e->sendId();

        Q_ASSERT(e->eventType() == QScxmlEvent::ExternalEvent);
        int id = d->m_qStateMachine->postDelayedEvent(e, e->delay());

        qCDebug(scxmlLog) << name() << ": delayed event" << e->name() << "(" << e << ") got id:" << id;
    } else {
        QStateMachine::EventPriority priority =
                e->eventType() == QScxmlEvent::ExternalEvent ? QStateMachine::NormalPriority
                                                             : QStateMachine::HighPriority;

        qCDebug(scxmlLog) << "queueing event" << e->name() << "in" << name();
        if (d->m_qStateMachine->isRunning())
            d->m_qStateMachine->postEvent(e, priority);
        else
            d->m_qStateMachine->queueEvent(e, priority);
    }
}

void StateMachine::submitEvent(const QByteArray &event)
{
    QScxmlEvent *e = new QScxmlEvent;
    e->setName(event);
    e->setEventType(QScxmlEvent::ExternalEvent);
    submitEvent(e);
}

void StateMachine::submitEvent(const QByteArray &event, const QVariant &data)
{
    QVariant incomingData = data;
    if (incomingData.canConvert<QJSValue>()) {
        incomingData = incomingData.value<QJSValue>().toVariant();
    }

    QScxmlEvent *e = new QScxmlEvent;
    e->setName(event);
    e->setEventType(QScxmlEvent::ExternalEvent);
    e->setData(incomingData);
    submitEvent(e);
}

void StateMachine::cancelDelayedEvent(const QByteArray &sendid)
{
    Q_D(StateMachine);

    int id = d->m_qStateMachine->eventIdForDelayedEvent(sendid);

    qCDebug(scxmlLog) << name() << ": canceling event" << sendid << "with id" << id;

    if (id != -1)
        d->m_qStateMachine->cancelDelayedEvent(id);
}

void Internal::WrappedQStateMachine::queueEvent(QScxmlEvent *event, EventPriority priority)
{
    Q_D(WrappedQStateMachine);

    qCDebug(scxmlLog) << d->m_table->name() << ": queueing event" << event->name();

    if (!d->m_queuedEvents)
        d->m_queuedEvents = new QVector<WrappedQStateMachinePrivate::QueuedEvent>();
    d->m_queuedEvents->append(WrappedQStateMachinePrivate::QueuedEvent(event, priority));
}

void Internal::WrappedQStateMachine::submitQueuedEvents()
{
    Q_D(WrappedQStateMachine);

    qCDebug(scxmlLog) << d->m_table->name() << ": submitting queued events";

    if (d->m_queuedEvents) {
        foreach (const WrappedQStateMachinePrivate::QueuedEvent &e, *d->m_queuedEvents)
            postEvent(e.event, e.priority);
        delete d->m_queuedEvents;
        d->m_queuedEvents = Q_NULLPTR;
    }
}

int Internal::WrappedQStateMachine::eventIdForDelayedEvent(const QByteArray &scxmlEventId)
{
    Q_D(WrappedQStateMachine);
    return d->eventIdForDelayedEvent(scxmlEventId);
}

bool StateMachine::isLegalTarget(const QString &target) const
{
    return target.startsWith(QLatin1Char('#'));
}

bool StateMachine::isDispatchableTarget(const QString &target) const
{
    if (isInvoked() && target == QStringLiteral("#_parent"))
        return true;
    return target == QStringLiteral("#_internal")
            || target == QStringLiteral("#_scxml_%1").arg(sessionId());
}

void StateMachine::registerService(QScxmlInvokableService *service)
{
    Q_D(StateMachine);
    Q_ASSERT(!d->m_registeredServices.contains(service));
    d->m_registeredServices.append(service);
}

void StateMachine::unregisterService(QScxmlInvokableService *service)
{
    Q_D(StateMachine);
    int idx =  d->m_registeredServices.indexOf(service);
    Q_ASSERT(idx >= 0);
    d->m_registeredServices.remove(idx);
}

void StateMachine::onFinished()
{
    // The final state is also a stable state.
    emit reachedStableState(true);
}

void StateMachine::start()
{
    Q_D(StateMachine);

    d->m_qStateMachine->start();
}

} // namespace Scxml

QT_END_NAMESPACE
