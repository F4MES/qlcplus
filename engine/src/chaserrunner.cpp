/*
  Q Light Controller Plus
  chaserrunner.cpp

  Copyright (c) Heikki Junnila
                Massimo Callegari
                Jano Svitok

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <QElapsedTimer>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <QDebug>
#include <cmath>

#include "chaserrunner.h"
#include "mastertimer.h"
#include "chaserstep.h"
#include "qlcmacros.h"
#include "chaser.h"
#include "scene.h"
#include "doc.h"

ChaserRunner::ChaserRunner(const Doc *doc, const Chaser *chaser, quint32 startTime)
    : QObject(NULL)
    , m_doc(doc)
    , m_chaser(chaser)
    , m_updateOverrideSpeeds(false)
    , m_startOffset(0)
    , m_lastRunStepIdx(-1)
    , m_lastFunctionID(Function::invalidId())
    , m_roundTime(new QElapsedTimer())
    , m_order()
    , m_beatMs(0)
    , m_beatDurationMs(0)
    , m_nextLinkBeatStart(-1)
    , m_beatCarryMs(0)
{
    Q_ASSERT(chaser != NULL);

    m_pendingAction.m_action = ChaserNoAction;
    m_pendingAction.m_masterIntensity = 1.0;
    m_pendingAction.m_stepIntensity = 1.0;
    m_pendingAction.m_fadeMode = Chaser::FromFunction;
    m_pendingAction.m_stepIndex = -1;

    if (startTime > 0)
    {
        qDebug() << "[ChaserRunner] startTime:" << startTime;
        int idx = 0;
        quint32 stepsTime = 0;
        foreach (ChaserStep step, chaser->steps())
        {
            uint duration = m_chaser->durationMode() == Chaser::Common ? m_chaser->duration() : step.duration;

            if (startTime < stepsTime + duration)
            {
                m_pendingAction.m_action = ChaserSetStepIndex;
                m_pendingAction.m_stepIndex = idx;
                m_startOffset = startTime - stepsTime;
                qDebug() << "[ChaserRunner] Starting from step:" << idx;
                break;
            }
            idx++;
            stepsTime += duration;
        }
    }

    m_direction = m_chaser->direction();
    connect(chaser, SIGNAL(changed(quint32)), this, SLOT(slotChaserChanged()));
    m_roundTime->restart();

    fillOrder();
}

ChaserRunner::~ChaserRunner()
{
    clearRunningList();
    delete m_roundTime;
}

/****************************************************************************
 * Speed
 ****************************************************************************/

void ChaserRunner::slotChaserChanged()
{
    // The runner lives on the GUI thread, so this runs there - a Speed/nudge
    // slider fires it many times a second - while write() walks and deletes
    // m_runnerSteps on the timer thread. Only raise a flag here.
    m_chaserChanged.storeRelease(1);
}

void ChaserRunner::applyChaserChange()
{
    // Handle (possible) speed change on the next write() pass
    m_updateOverrideSpeeds = true;
    QList<ChaserRunnerStep*> delList;
    foreach (ChaserRunnerStep *step, m_runnerSteps)
    {
        // m_fid, not m_function->id(): when a Function is deleted from the
        // Doc, this runs after it has been freed
        if (!m_chaser->steps().contains(ChaserStep(step->m_fid)))
        {
            // Disappearing function: remove step
            delList.append(step);
        }
        else
        {
            // Recalculate the speed of each running step
            step->m_fadeIn = stepFadeIn(step->m_index);
            step->m_fadeOut = stepFadeOut(step->m_index);
            step->m_duration = stepDuration(step->m_index);
        }
    }
    foreach (ChaserRunnerStep *step, delList)
    {
        // Only stop a Function that still exists (removed from the chaser,
        // not deleted from the Doc)
        Function *f = m_doc->function(step->m_fid);
        if (f != NULL && f == step->m_function)
            f->stop(functionParent());
        m_runnerSteps.removeAll(step);
        delete step;
    }
}

uint ChaserRunner::stepFadeIn(int stepIdx) const
{
    uint speed = 0;
    if (m_chaser->overrideFadeInSpeed() != Function::defaultSpeed())
    {
        // Override speed is used when another function has started the chaser,
        // i.e. chaser inside a chaser that wants to impose its own fade in speed
        // to its members.
        speed = m_chaser->overrideFadeInSpeed();
    }
    else
    {
        switch (m_chaser->fadeInMode())
        {
            case Chaser::Common:
                // All steps' fade in speed is dictated by the chaser
                speed = m_chaser->fadeInSpeed();
            break;
            case Chaser::PerStep:
                // Each step specifies its own fade in speed
                if (stepIdx >= 0 && stepIdx < m_chaser->stepsCount())
                    speed = m_chaser->steps().at(stepIdx).fadeIn;
                else
                    speed = Function::defaultSpeed();
            break;
            default:
            case Chaser::Default:
                // Don't touch members' fade in speed at all
                speed = Function::defaultSpeed();
            break;
        }
    }

    return speed;
}

uint ChaserRunner::stepFadeOut(int stepIdx) const
{
    uint speed = 0;
    if (m_chaser->overrideFadeOutSpeed() != Function::defaultSpeed())
    {
        // Override speed is used when another function has started the chaser,
        // i.e. chaser inside a chaser that wants to impose its own fade out speed
        // to its members.
        speed = m_chaser->overrideFadeOutSpeed();
    }
    else
    {
        switch (m_chaser->fadeOutMode())
        {
            case Chaser::Common:
                // All steps' fade out speed is dictated by the chaser
                speed = m_chaser->fadeOutSpeed();
            break;
            case Chaser::PerStep:
                // Each step specifies its own fade out speed
                if (stepIdx >= 0 && stepIdx < m_chaser->stepsCount())
                    speed = m_chaser->steps().at(stepIdx).fadeOut;
                else
                    speed = Function::defaultSpeed();
            break;
            default:
            case Chaser::Default:
                // Don't touch members' fade out speed at all
                speed = Function::defaultSpeed();
            break;
        }
    }

    return speed;
}

uint ChaserRunner::stepDuration(int stepIdx) const
{
    uint speed = 0;
    if (m_chaser->overrideDuration() != Function::defaultSpeed())
    {
        // Override speed is used when another function has started the chaser,
        // i.e. chaser inside a chaser that wants to impose its own duration
        // to its members.
        speed = m_chaser->overrideDuration();
    }
    else
    {
        switch (m_chaser->durationMode())
        {
            default:
            case Chaser::Default:
            case Chaser::Common:
                // All steps' duration is dictated by the chaser
                speed = m_chaser->duration();
            break;
            case Chaser::PerStep:
                // Each step specifies its own duration
                if (stepIdx >= 0 && stepIdx < m_chaser->stepsCount())
                    speed = m_chaser->steps().at(stepIdx).duration;
                else
                    speed = m_chaser->duration();
            break;
        }
    }

    return speed;
}

uint ChaserRunner::stepFadeUnits(const Function *func, uint fade) const
{
    // Overlap starts a step function in its OWN tempo (so it can finish on
    // its own), but the chaser's fades are in the chaser's units: a 1/4-beat
    // fade is 250 in Beats and must not reach a Time function as 250 ms.
    // B24_TRACK_FADES: ... and so does a chaser the TRACK engine started - a
    // Time scene read the chaser's 24-beat fade as 24000 ms, so an "AUTO Bars
    // Tilt" glide never reached its target and the tilt jumped at every step.
    // A chaser from the Virtual Console is left exactly as it always ran
    // (Tobias, 2026-09-28).
    const bool convert = m_chaser->overlapMode() || m_chaser->startedByTrackEngine();
    if (convert == false || func == NULL ||
        m_chaser->tempoType() != Function::Beats || func->tempoType() != Function::Time ||
        fade == Function::defaultSpeed())
        return fade;
    return Function::beatsToTime(fade, m_doc->masterTimer()->beatTimeDuration());
}

/** Beat length of a Beats-mode step on the Link grid. Durations are uint
 *  beats*1000, so 1/16 (62.5), 1/3, 1/6, 1/12 cannot be held exactly, and a
 *  chain of rounded steps walks off the beat. Only a duration that IS the
 *  rounded 1/n value (within 1 unit) is taken as 1/n; any other duration
 *  (a nudge, 3/4, 2/3, dotted values) keeps its exact length.
 *  0 for "no grid" (zero or infinite duration). */
static qreal linkSpacing(uint duration)
{
    if (duration == 0 || duration == Function::infiniteSpeed())
        return 0.0;
    qreal spacing = qreal(duration) / 1000.0;
    if (duration < 1000)
    {
        qreal perBeat = 1000.0 / qreal(duration);
        qreal n = qRound(perBeat);
        if (n >= 1.0 && qAbs(qreal(duration) - 1000.0 / n) < 1.0)
            spacing = 1.0 / n;
    }
    return spacing;
}

/****************************************************************************
 * Step control
 ****************************************************************************/

void ChaserRunner::setAction(const ChaserAction &action)
{
    // apply the actions that can be applied immediately
    switch (action.m_action)
    {
        case ChaserNoAction:
            m_pendingAction.m_masterIntensity = action.m_masterIntensity;
            m_pendingAction.m_stepIntensity = action.m_stepIntensity;
        break;

        case ChaserStopStep:
        {
            bool stopped = false;

            foreach (ChaserRunnerStep *step, m_runnerSteps)
            {
                if (action.m_stepIndex == step->m_index)
                {
                    qDebug() << "[ChaserRunner] Stopping step idx:" << action.m_stepIndex << "(running:" << m_runnerSteps.count() << ")";
                    m_lastFunctionID = step->m_function->type() == Function::SceneType ? step->m_function->id() : Function::invalidId();
                    step->m_function->stop(functionParent());
                    m_runnerSteps.removeOne(step);
                    delete step;
                    stopped = true;
                }
            }

            if (stopped && m_runnerSteps.size() == 1)
            {
                ChaserRunnerStep *lastStep = m_runnerSteps.at(0);
                m_lastRunStepIdx = lastStep->m_index;
                emit currentStepChanged(m_lastRunStepIdx);
            }
        }
        break;

        // copy to pending action. Will be processed at the next write call
        default:
            m_pendingAction.m_stepIndex = action.m_stepIndex;
            m_pendingAction.m_masterIntensity = action.m_masterIntensity;
            m_pendingAction.m_stepIntensity = action.m_stepIntensity;
            m_pendingAction.m_fadeMode = action.m_fadeMode;
            m_pendingAction.m_action = action.m_action;
        break;
    }
}

void ChaserRunner::tap()
{
    if (uint(m_roundTime->elapsed()) >= (stepDuration(m_lastRunStepIdx) / 4))
        m_pendingAction.m_action = ChaserNextStep;
}

int ChaserRunner::currentStepIndex() const
{
    return m_lastRunStepIdx;
}

int ChaserRunner::runningStepsNumber() const
{
    return m_runnerSteps.count();
}

ChaserRunnerStep *ChaserRunner::currentRunningStep() const
{
    if (m_runnerSteps.count() > 0)
        return m_runnerSteps.at(0);
    return NULL;
}

int ChaserRunner::computeNextStep(int currentStep) const
{
    int nextStep = currentStep;

    if (m_chaser->runOrder() == Function::Random)
    {
        nextStep = m_order.indexOf(nextStep);
        if (nextStep == -1)
        {
            qDebug() << "[ChaserRunner] steps order not found";
            nextStep = currentStep;
        }
    }

    // Next step
    if (m_direction == Function::Forward)
    {
        nextStep++;
    }
    else
    {
        nextStep--;
    }

    if (nextStep < m_chaser->stepsCount() && nextStep >= 0)
    {
        if (m_chaser->runOrder() == Function::Random)
        {
            nextStep = randomStepIndex(nextStep);
        }
        return nextStep; // In the middle of steps. No need to go any further.
    }

    if (m_chaser->runOrder() == Function::SingleShot)
    {
        return -1; // Forward or Backward SingleShot has been completed.
    }
    else if (m_chaser->runOrder() == Function::Loop)
    {
        if (m_direction == Function::Forward)
        {
            if (nextStep >= m_chaser->stepsCount())
                nextStep = 0;
            else
                nextStep = m_chaser->stepsCount() - 1; // Used by CueList with manual prev
        }
        else // Backward
        {
            if (nextStep < 0)
                nextStep = m_chaser->stepsCount() - 1;
            else
                nextStep = 0;
        }
    }
    else if (m_chaser->runOrder() == Function::Random)
    {
        nextStep = randomStepIndex(nextStep);
    }
    else // Ping Pong
    {
        // Change direction, but don't run the first/last step twice.
        if (m_direction == Function::Forward)
        {
            nextStep = m_chaser->stepsCount() - 2;
        }
        else // Backwards
        {
            nextStep = 1;
        }

        // Make sure we don't go beyond limits.
        nextStep = CLAMP(nextStep, 0, m_chaser->stepsCount() - 1);
    }

    return nextStep;
}

void ChaserRunner::shuffle(QVector<int> & data)
{
   int n = data.size();
   for (int i = n - 1; i > 0; --i)
   {
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
      qSwap(data[i], data[qrand() % (i + 1)]);
#else
      qSwap(data[i], data[QRandomGenerator::global()->generate() % (i + 1)]);
#endif
   }
}

int ChaserRunner::randomStepIndex(int step) const
{
   if (m_chaser->runOrder() == Function::Random && step >= 0 && step < m_order.size())
       return m_order[step];

   return step;
}

void ChaserRunner::fillOrder()
{
    fillOrder(m_chaser->stepsCount());
}

void ChaserRunner::fillOrder(int size)
{
   m_order.resize(size);
   for (int i = 0; i < size; ++i)
       m_order[i] = i;

   shuffle(m_order);
}

/****************************************************************************
 * Intensity
 ****************************************************************************/

void ChaserRunner::adjustStepIntensity(qreal fraction, int requestedStepIndex, int fadeControl)
{
    fraction = CLAMP(fraction, qreal(0.0), qreal(1.0));

    //qDebug() << "Adjust intensity" << fraction << "step:" << requestedStepIndex << "fade:" << fadeControl;

    int stepIndex = requestedStepIndex;
    if (stepIndex == -1)
    {
        // store the intensity to be applied at the next step startup
        m_pendingAction.m_masterIntensity = fraction;

        foreach (ChaserRunnerStep *step, m_runnerSteps)
        {
            if (step == NULL || step->m_function == NULL)
                continue;

            step->m_masterIntensity = fraction;
            if (step->m_function->type() == Function::SceneType)
            {
                Scene *scene = qobject_cast<Scene *>(step->m_function);
                scene->adjustAttribute(fraction, step->m_pIntensityOverrideId);
            }
            else
            {
                step->m_function->adjustAttribute(fraction * step->m_stepIntensity, step->m_intensityOverrideId);
            }
        }

        return;
    }

    foreach (ChaserRunnerStep *step, m_runnerSteps)
    {
        if (stepIndex == step->m_index && step->m_function != NULL)
        {
            step->m_stepIntensity = fraction;
            if (step->m_function->type() == Function::SceneType)
                step->m_function->adjustAttribute(fraction, step->m_intensityOverrideId);
            else
                step->m_function->adjustAttribute(step->m_masterIntensity * fraction, step->m_intensityOverrideId);
            return;
        }
    }

    // No need to start a new step if it is not wanted
    if (requestedStepIndex == -1)
        return;

    // Don't start a step with an intensity of zero
    if (fraction == qreal(0.0))
        return;

    // not found ? It means we need to start a new step and crossfade kicks in !
    startNewStep(stepIndex, m_doc->masterTimer(), m_pendingAction.m_masterIntensity, fraction, fadeControl);
}

/****************************************************************************
 * Running
 ****************************************************************************/

void ChaserRunner::clearRunningList()
{
    // A pending chaser edit may name a deleted Function: apply it first
    // (a paused chaser does not write(), and postRun() lands here)
    if (m_chaserChanged.testAndSetAcquire(1, 0))
        applyChaserChange();

    // empty the running queue
    foreach (ChaserRunnerStep *step, m_runnerSteps)
    {
        if (step->m_function)
        {
            // restore the original Function fade out time
            step->m_function->setOverrideFadeOutSpeed(stepFadeUnits(step->m_function, stepFadeOut(step->m_index)));
            step->m_function->stop(functionParent(), m_chaser->type() == Function::SequenceType);
            m_lastFunctionID = step->m_function->type() == Function::SceneType ? step->m_function->id() : Function::invalidId();
        }
        delete step;
    }
    m_runnerSteps.clear();
    // Manual jump (next/prev/goto): drop any carried Link boundary so the
    // next step re-snaps to Link's grid instead of continuing the old chain.
    m_nextLinkBeatStart = -1;
    // A jump also drops the sub-beat overrun carried to the next step
    m_beatCarryMs = 0;
}

void ChaserRunner::startNewStep(int index, MasterTimer *timer, qreal mIntensity, qreal sIntensity,
                                int fadeControl, quint32 elapsed)
{
    if (m_chaser == NULL || m_chaser->stepsCount() == 0)
        return;

    if (index < 0 || index >= m_chaser->stepsCount())
        index = 0; // fallback to the first step

    ChaserStep step(m_chaser->steps().at(index));
    Function *func = m_doc->function(step.fid);
    if (func == NULL)
        return;

    ChaserRunnerStep *newStep = new ChaserRunnerStep();
    newStep->m_index = index;
    newStep->m_function = func;
    newStep->m_fid = func->id();
    newStep->m_masterIntensity = mIntensity;
    newStep->m_stepIntensity = sIntensity;
    newStep->m_intensityOverrideId = Function::invalidAttributeId();
    newStep->m_pIntensityOverrideId = Function::invalidAttributeId();

    // check if blending between Scenes is needed
    if (m_lastFunctionID != Function::invalidId() &&
        func->type() == Function::SceneType)
    {
        Scene *scene = qobject_cast<Scene *>(func);
        scene->setBlendFunctionID(m_lastFunctionID);
    }

    // this happens only during crossfades
    if (m_runnerSteps.count())
    {
        ChaserRunnerStep *lastStep = m_runnerSteps.last();
        if (lastStep->m_function &&
            lastStep->m_function->type() == Function::SceneType &&
            func->type() == Function::SceneType)
        {
            Scene *lastScene = qobject_cast<Scene *>(lastStep->m_function);
            lastScene->setBlendFunctionID(Function::invalidId());
            Scene *scene = qobject_cast<Scene *>(func);
            scene->setBlendFunctionID(lastStep->m_function->id());
        }
    }

    switch (fadeControl)
    {
        case Chaser::FromFunction:
            newStep->m_fadeIn = stepFadeIn(index);
            newStep->m_fadeOut = stepFadeOut(index);
        break;
        case Chaser::Blended:
            newStep->m_fadeIn = stepFadeIn(index);
            newStep->m_fadeOut = stepFadeOut(index);
        break;
        case Chaser::Crossfade:
            newStep->m_fadeIn = 0;
            newStep->m_fadeOut = 0;
        break;
        case Chaser::BlendedCrossfade:
            newStep->m_fadeIn = 0;
            newStep->m_fadeOut = 0;
        break;
    }

    newStep->m_duration = stepDuration(index);

    if (m_startOffset != 0)
        newStep->m_elapsed = m_startOffset + MasterTimer::tick();
    else
        newStep->m_elapsed = MasterTimer::tick() + elapsed;
    newStep->m_elapsedBeats = 0; //(newStep->m_elapsed / timer->beatTimeDuration()) * 1000;
    // The first write() of this step comes one tick after it started, just as
    // m_elapsed already counts that tick: start the sub-beat clock one tick
    // back, plus whatever the previous sub-beat step overran its end by, so a
    // fractional step neither runs a tick long nor rounds every step up.
    newStep->m_elapsedAtLastBeat = newStep->m_elapsed - MasterTimer::tick() - m_beatCarryMs;
    m_beatCarryMs = 0;

    // Ableton Link sub-beat anchoring. Either continue from the previous
    // step's exact boundary (carry-forward, set when the last step advanced)
    // so the chain never drifts, or - on a fresh start - snap to Link's
    // absolute sub-beat grid so the chase 'ends' land on whole beats.
    if (timer != NULL && timer->linkEnabled())
    {
        if (m_nextLinkBeatStart >= 0)
        {
            newStep->m_linkBeatStart = m_nextLinkBeatStart;
        }
        else
        {
            qreal spacing = linkSpacing(newStep->m_duration);
            qreal now = timer->linkBeat();
            // std::floor, not a qint64 cast: Link beats can be negative
            if (spacing <= 0.0)
                newStep->m_linkBeatStart = now;
            else if (spacing >= 1.0)
                // Whole-beat (or multi-beat) steps: snap to the step grid so the
                // first transition lands on a beat (the chase aligns itself).
                newStep->m_linkBeatStart = std::floor(now / spacing) * spacing;
            else
                // Sub-beat (fractional) steps: begin the pattern on the NEXT whole
                // beat, so a chase triggered between beats still locks its cycle to
                // the beat instead of running off-grid. (epsilon absorbs fp jitter)
                newStep->m_linkBeatStart = std::floor(now - 0.001) + 1.0;
        }
    }
    else
    {
        newStep->m_linkBeatStart = 0;
    }
    m_nextLinkBeatStart = -1;

    m_startOffset = 0;

    if (m_chaser->type() == Function::SequenceType)
    {
        Scene *s = qobject_cast<Scene*>(func);
        // blind == true is a workaround to reuse the same scene
        // without messing up the previous values
        for (int i = 0; i < step.values.count(); i++)
            s->setValue(step.values.at(i), true);
    }

    qDebug() << "[ChaserRunner] Starting step" << index << "fade in" << newStep->m_fadeIn
             << "fade out" << newStep->m_fadeOut << "intensity" << mIntensity
             << "fadeMode" << fadeControl;

    // Set intensity before starting the function. Otherwise the intensity
    // might momentarily jump too high.
    if (func->type() == Function::SceneType)
    {
        Scene *scene = qobject_cast<Scene *>(func);
        newStep->m_intensityOverrideId = func->requestAttributeOverride(Function::Intensity, sIntensity);
        newStep->m_pIntensityOverrideId = scene->requestAttributeOverride(Scene::ParentIntensity, mIntensity);
        qDebug() << "[ChaserRunner] Set step intensity:" << sIntensity << ", master:" << mIntensity;
    }
    else
    {
        newStep->m_intensityOverrideId = func->requestAttributeOverride(Function::Intensity, mIntensity * sIntensity);
    }

    // Overlap: a function still running from an earlier lap must fire again.
    // start() returns early while this chaser is still one of its sources.
    // preserveAttributes: the restart's postRun() must not wipe the intensity
    // override requested just above.
    if (m_overlapFids.removeAll(func->id()) > 0)
        func->stop(functionParent(), true);

    // Start the fire up!
    func->start(timer, functionParent(), 0, stepFadeUnits(func, newStep->m_fadeIn),
                stepFadeUnits(func, newStep->m_fadeOut),
                func->defaultSpeed(),
                m_chaser->overlapMode() ? Function::Original : m_chaser->tempoType());
    m_runnerSteps.append(newStep);
    m_roundTime->restart();
}

int ChaserRunner::getNextStepIndex()
{
    int currentStepIndex = m_lastRunStepIdx;

    if (m_chaser->runOrder() == Function::Random)
    {
        currentStepIndex = m_order.indexOf(currentStepIndex);
        if (currentStepIndex == -1)
        {
            qDebug() << "[ChaserRunner] steps order not found";
            currentStepIndex = m_lastRunStepIdx;
        }
    }

    if (currentStepIndex == -1 &&
        m_chaser->direction() == Function::Backward)
            currentStepIndex = m_chaser->stepsCount();

    // Handle reverse Ping Pong at boundaries
    if (m_chaser->runOrder() == Function::PingPong &&
        m_pendingAction.m_action == ChaserPreviousStep)
    {
        if (currentStepIndex == 0)
            m_direction = Function::Backward;
        else if (currentStepIndex == m_chaser->stepsCount() - 1)
            m_direction = Function::Forward;
    }

    // Next step
    if (m_direction == Function::Forward)
    {
        // "Previous" for a forward chaser is -1
        if (m_pendingAction.m_action == ChaserPreviousStep)
            currentStepIndex--;
        else
            currentStepIndex++;
    }
    else
    {
        // "Previous" for a backward scene is +1
        if (m_pendingAction.m_action == ChaserPreviousStep)
            currentStepIndex++;
        else
            currentStepIndex--;
    }

    if (currentStepIndex < m_chaser->stepsCount() && currentStepIndex >= 0)
    {
        if (m_chaser->runOrder() == Function::Random)
        {
            currentStepIndex = randomStepIndex(currentStepIndex);
        }
        return currentStepIndex; // In the middle of steps. No need to go any further.
    }

    if (m_chaser->runOrder() == Function::SingleShot)
    {
        return -1; // Forward or Backward SingleShot has been completed.
    }
    else if (m_chaser->runOrder() == Function::Loop)
    {
        if (m_direction == Function::Forward)
        {
            if (currentStepIndex >= m_chaser->stepsCount())
                currentStepIndex = 0;
            else
                currentStepIndex = m_chaser->stepsCount() - 1; // Used by CueList with manual prev
        }
        else // Backward
        {
            if (currentStepIndex < 0)
                currentStepIndex = m_chaser->stepsCount() - 1;
            else
                currentStepIndex = 0;
        }
    }
    else if (m_chaser->runOrder() == Function::Random)
    {
        fillOrder();
        if (m_direction == Function::Forward)
        {
            if (currentStepIndex >= m_chaser->stepsCount())
                currentStepIndex = 0;
            else
                currentStepIndex = m_chaser->stepsCount() - 1; // Used by CueList with manual prev
        }
        else // Backward
        {
            if (currentStepIndex < 0)
                currentStepIndex = m_chaser->stepsCount() - 1;
            else
                currentStepIndex = 0;
        }
        // Don't run the same function 2 times in a row
        while (currentStepIndex < m_chaser->stepsCount()
                && randomStepIndex(currentStepIndex) == m_lastRunStepIdx)
            ++currentStepIndex;
        currentStepIndex = randomStepIndex(currentStepIndex);
    }
    else // Ping Pong
    {
        // Change direction, but don't run the first/last step twice.
        if (m_direction == Function::Forward)
        {
            currentStepIndex = m_chaser->stepsCount() - 2;
            m_direction = Function::Backward;
        }
        else // Backwards
        {
            currentStepIndex = 1;
            m_direction = Function::Forward;
        }

        // Make sure we don't go beyond limits.
        currentStepIndex = CLAMP(currentStepIndex, 0, m_chaser->stepsCount() - 1);
    }

    return currentStepIndex;
}

void ChaserRunner::setPause(bool enable, QList<Universe *> universes)
{
    // Nothing to do
    if (m_chaser->stepsCount() == 0)
        return;

    qDebug() << "[ChaserRunner] processing pause request:" << enable;

    foreach (ChaserRunnerStep *step, m_runnerSteps)
        step->m_function->setPause(enable);

    // there might be a Scene fading out, so request pause
    // to faders bound to the Scene ID running on universes
    Function *f = m_doc->function(m_lastFunctionID);
    if (f != NULL && f->type() == Function::SceneType)
    {
        foreach (Universe *universe, universes)
            universe->setFaderPause(m_lastFunctionID, enable);
    }
}

FunctionParent ChaserRunner::functionParent() const
{
    return FunctionParent(FunctionParent::Function, m_chaser->id());
}

bool ChaserRunner::write(MasterTimer *timer, QList<Universe *> universes)
{
    // Nothing to do
    if (m_chaser->stepsCount() == 0)
        return false;

    // Chaser edits (speed, nudge, steps) are applied here, on the timer thread
    if (m_chaserChanged.testAndSetAcquire(1, 0))
        applyChaserChange();

    switch (m_pendingAction.m_action)
    {
        case ChaserNextStep:
        case ChaserPreviousStep:
            clearRunningList();
            // the actual action will be performed below, on startNewStep
        break;
        case ChaserSetStepIndex:
            if (m_pendingAction.m_stepIndex != -1)
            {
                clearRunningList();
                if (m_chaser->runOrder() == Function::Random)
                    m_lastRunStepIdx = randomStepIndex(m_pendingAction.m_stepIndex);
                else
                    m_lastRunStepIdx = m_pendingAction.m_stepIndex;

                qDebug() << "[ChaserRunner] Starting from step" << m_lastRunStepIdx << "@ offset" << m_startOffset;
                startNewStep(m_lastRunStepIdx, timer, m_pendingAction.m_masterIntensity,
                             m_pendingAction.m_stepIntensity, m_pendingAction.m_fadeMode);
                emit currentStepChanged(m_lastRunStepIdx);
            }
        break;
        case ChaserPauseRequest:
            setPause(m_pendingAction.m_fadeMode ? true : false, universes);
        break;
        default:
        break;
    }

    // Measure the actual interval between incoming beats, so the sub-beat
    // interpolation below tracks the real tempo instead of the nominal BPM
    // (prevents fractional steps from slowly drifting against the beat).
    if (m_chaser->tempoType() == Function::Beats)
    {
        m_beatMs += MasterTimer::tick();
        if (timer->isBeat())
        {
            // Smooth the measured interval over ~4 beats and reject clearly
            // off intervals (clock start/stop, dropped/extra beats), so the
            // sub-beat rate stays steady on a jittery (e.g. loopback) clock.
            int nominalBeat = timer->beatTimeDuration();
            if (nominalBeat <= 0 ||
                (m_beatMs > quint32(nominalBeat) / 2 && m_beatMs < quint32(nominalBeat) * 2))
            {
                if (m_beatDurationMs == 0)
                    m_beatDurationMs = m_beatMs;                          // seed
                else
                    m_beatDurationMs = (m_beatDurationMs * 3 + m_beatMs) / 4; // moving average
            }
            m_beatMs = 0;
        }
    }

    quint32 prevStepRoundElapsed = 0;
    int subBeatMs = 0;      // beat length the sub-beat interpolation used this tick

    foreach (ChaserRunnerStep *step, m_runnerSteps)
    {
        // Progress through the current step. In Beats mode this is measured in beat
        // units where 1000 == one full beat (so 500 == 1/2 beat, 250 == 1/4, 125 == 1/8).
        quint32 stepProgress = step->m_elapsedBeats;

        if (m_chaser->tempoType() == Function::Beats && timer->linkEnabled())
        {
            // Ableton Link exposes a continuous, shared beat timeline. Derive the
            // step progress straight from it so BOTH whole-beat and sub-beat
            // (fractional) steps stay exactly locked to Link's grid: each step
            // advances precisely m_duration/1000 beats after its anchored start,
            // and the per-step boundary is carried forward (below) so the error
            // never accumulates. (The old interpolation drifted on sub-beats.)
            qreal spacing = linkSpacing(step->m_duration);
            qreal delta = timer->linkBeat() - step->m_linkBeatStart;
            // No anchor lies more than a beat (+ a step) ahead of Link: this is
            // the timeline jumping back (session join, a peer forcing the beat).
            // Re-anchor on the new grid rather than hold the step until Link is
            // back where it was.
            if (delta < -(1.0 + spacing))
            {
                qreal now = timer->linkBeat();
                step->m_linkBeatStart = spacing > 0.0 ? std::floor(now / spacing) * spacing : now;
                delta = now - step->m_linkBeatStart;
            }
            // Judge the end against the exact grid spacing (1/16 is stored as
            // 62 or 63, not 62.5)
            if (delta <= 0)
                stepProgress = 0;
            else if (spacing > 0.0 && delta >= spacing)
                stepProgress = step->m_duration;
            else
                stepProgress = qMin(quint32(delta * 1000.0),
                                    step->m_duration > 0 ? step->m_duration - 1 : 0);
        }
        else if (m_chaser->tempoType() == Function::Beats)
        {
            // Whole beats are driven by the (internal or external/MIDI) beat clock,
            // so step changes stay phase-locked to the incoming beats.
            if (timer->isBeat())
            {
                step->m_elapsedBeats += 1000;
                step->m_elapsedAtLastBeat = step->m_elapsed;
            }

            // Interpolate the position *within* the current beat from the elapsed time
            // and the current beat duration. This is what makes sub-beat (fractional)
            // step durations possible while remaining locked to the beat clock.
            quint32 beatFraction = 0;
            int beatDuration = timer->beatTimeDuration();
            // Prefer the measured beat interval when it is sane (within 2x of
            // the nominal), so sub-beats stay locked to the real incoming tempo.
            // Ableton Link gives the exact tempo in beatDuration already, so
            // skip the smoothed measured interval (it would only add lag).
            if (timer->linkEnabled() == false &&
                m_beatDurationMs > 0 && beatDuration > 0 &&
                m_beatDurationMs > quint32(beatDuration) / 2 &&
                m_beatDurationMs < quint32(beatDuration) * 2)
                beatDuration = int(m_beatDurationMs);
            if (beatDuration > 0)
            {
                quint64 frac = (quint64(step->m_elapsed - step->m_elapsedAtLastBeat) * 1000)
                               / quint32(beatDuration);
                // The whole-beat boundary is owned by isBeat() above, so never let the
                // interpolated fraction reach (or overtake) the next beat on its own.
                beatFraction = frac > 999 ? 999 : quint32(frac);
            }
            stepProgress = step->m_elapsedBeats + beatFraction;
            subBeatMs = beatDuration;

            qDebug() << "[ChaserRunner] Function" << step->m_function->name() << "duration:" << step->m_duration << "beats:" << stepProgress;
        }

        if (step->m_duration != Function::infiniteSpeed() &&
            ((m_chaser->tempoType() == Function::Time && step->m_elapsed >= step->m_duration) ||
             (m_chaser->tempoType() == Function::Beats && stepProgress >= step->m_duration)))
        {
            if (step->m_duration != 0)
                prevStepRoundElapsed = step->m_elapsed % step->m_duration;

            // Beats without Link: credit what this sub-beat step ran past its
            // end to the next one, so tick rounding does not pile up across the
            // beat. A beat tick re-phases everything to 0 instead.
            if (subBeatMs > 0 && timer->isBeat() == false && stepProgress > step->m_duration)
                m_beatCarryMs = qMin(quint32(subBeatMs),
                                     quint32(quint64(stepProgress - step->m_duration) * quint32(subBeatMs) / 1000));

            // Link: carry the exact next-step boundary forward so successive
            // sub-beat steps stay locked to Link's grid without accumulating
            // per-step rounding drift.
            if (m_chaser->tempoType() == Function::Beats && timer->linkEnabled() && step->m_duration != 0)
            {
                qreal spacing = linkSpacing(step->m_duration);
                qreal next = step->m_linkBeatStart + spacing;
                // Back onto the beat grid: drops the fp residue of a long chain,
                // and pulls the pattern back onto the beat once a nudge is
                // released. Only when every step has this length and the step
                // grid IS the beat grid (1/n or whole beats) - otherwise the
                // rounding would change step lengths (PerStep, 3/4, 2/3, nudged).
                bool uniformLen = m_chaser->overrideDuration() != Function::defaultSpeed() ||
                                  m_chaser->durationMode() != Chaser::PerStep;
                qreal perBeat = 1.0 / spacing;
                bool beatGrid = qAbs(perBeat - qRound(perBeat)) < 1e-6 ||
                                qAbs(spacing - qRound(spacing)) < 1e-6;
                if (uniformLen && beatGrid)
                    next = std::floor(next / spacing + 0.5) * spacing;

                // If that boundary is ALSO already over (chaser paused/frozen while
                // Link kept running, Link enabled mid-run, a forward jump of the Link
                // timeline, a shortened step, or a step shorter than one tick), skip
                // the lost steps on the same grid instead of racing one step per tick.
                // behind < 0 (Link jumped backwards) is left alone here.
                qreal behind = timer->linkBeat() - next;
                if (behind >= spacing)
                {
                    // behind/spacing >= 1, so truncation == floor
                    qint64 lost = qint64(behind / spacing);
                    next += qreal(lost) * spacing;

                    // Keep the pattern phase-locked to the grid: advance the step
                    // index by the lost count too, so step 0 stays on the beat.
                    // Only meaningful for evenly spaced, cyclic patterns.
                    bool uniform = m_chaser->overrideDuration() != Function::defaultSpeed() ||
                                   m_chaser->durationMode() != Chaser::PerStep;
                    int count = m_chaser->stepsCount();
                    qint64 period = 0;
                    if (m_chaser->runOrder() == Function::Loop)
                        period = count;
                    else if (m_chaser->runOrder() == Function::PingPong)
                        period = qMax(2, 2 * count - 2);

                    if (uniform && period > 0)
                    {
                        for (qint64 i = lost % period; i > 0; i--)
                        {
                            int idx = getNextStepIndex();
                            if (idx == -1)
                                break;
                            m_lastRunStepIdx = idx;
                        }
                    }
                }
                m_nextLinkBeatStart = next;
            }

            m_lastFunctionID = step->m_function->type() == Function::SceneType ? step->m_function->id() : Function::invalidId();
            // Overlap mode: leave the current step's function running (it finishes
            // on its own) so successive inner functions overlap.
            if (m_chaser->overlapMode() == false)
                step->m_function->stop(functionParent(), m_chaser->type() == Function::SequenceType);
            else if (m_overlapFids.contains(step->m_function->id()) == false)
                m_overlapFids.append(step->m_function->id());
            m_runnerSteps.removeOne(step);
            delete step;
        }
        else
        {
            if (step->m_elapsed < UINT_MAX)
                step->m_elapsed += MasterTimer::tick();

            // When the speeds of the chaser change, they need to be updated to the lower
            // level (only current function) as well. Otherwise the new speeds would take
            // effect only on the next step change.
            if (m_updateOverrideSpeeds == true)
            {
                m_updateOverrideSpeeds = false;
                if (step->m_function != NULL)
                {
                    step->m_function->setOverrideFadeInSpeed(stepFadeUnits(step->m_function, step->m_fadeIn));
                    step->m_function->setOverrideFadeOutSpeed(stepFadeUnits(step->m_function, step->m_fadeOut));
                }
            }
        }
    }

    if (m_runnerSteps.isEmpty())
    {
        m_lastRunStepIdx = getNextStepIndex();
        if (m_lastRunStepIdx != -1)
        {
            int blend = m_pendingAction.m_action == ChaserNoAction ? Chaser::FromFunction : m_pendingAction.m_fadeMode;

            startNewStep(m_lastRunStepIdx, timer, m_pendingAction.m_masterIntensity,
                         m_pendingAction.m_stepIntensity, blend, prevStepRoundElapsed);
            emit currentStepChanged(m_lastRunStepIdx);
        }
        else
        {
            // Natural end: overlapping tails finish on their own, as before
            m_overlapFids.clear();
            m_pendingAction.m_action = ChaserNoAction;
            return false;
        }
    }

    m_pendingAction.m_action = ChaserNoAction;
    return true;
}

void ChaserRunner::postRun(MasterTimer *timer, QList<Universe*> universes)
{
    Q_UNUSED(universes);
    Q_UNUSED(timer);

    qDebug() << Q_FUNC_INFO;
    clearRunningList();

    // Overlap mode, stopped from outside: what the steps left running goes
    // with the chaser
    foreach (quint32 fid, m_overlapFids)
    {
        Function *f = m_doc->function(fid);
        if (f != NULL)
            f->stop(functionParent());
    }
    m_overlapFids.clear();
}
