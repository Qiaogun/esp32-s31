#include "OuORuntime.h"

#include <math.h>
#include <string.h>
#include <strings.h>

void CtfObject::reset() {
    x = startX;
    y = startY;
    scaleX = 1.0f;
    scaleY = 1.0f;
    angle = 0.0f;
    dir = 0;
    visible = true;
    for (int i = 0; i < 16; ++i) {
        av[i] = 0.0f;
        flags[i] = false;
    }
}

OuORuntime::OuORuntime()
    : eyeRight_{ObjectKind::Eye, 1029.0f, 301.0f},
      eyeLeft_{ObjectKind::Eye, 252.0f, 301.0f},
      topLeft_{ObjectKind::TopLid, 252.0f, 97.0f},
      bottomLeft_{ObjectKind::BottomLid, 252.0f, 505.0f},
      topRight_{ObjectKind::TopLid, 1029.0f, 97.0f},
      bottomRight_{ObjectKind::BottomLid, 1029.0f, 505.0f},
      atLeft_{ObjectKind::At, 413.0f, 132.0f},
      atRight_{ObjectKind::At, 859.0f, 168.0f},
      vLeft_{ObjectKind::V, 252.0f, 301.0f},
      vRight_{ObjectKind::V, 1029.0f, 301.0f},
      blushRight_{ObjectKind::Blush, 1128.0f, 360.0f},
      blushLeft_{ObjectKind::Blush, 152.0f, 360.0f},
      sbLeft_{ObjectKind::Sb, 252.0f, 760.0f},
      sbRight_{ObjectKind::Sb, 1029.0f, 760.0f},
      mouth_{ObjectKind::Mouth, 640.0f, 465.0f},
      poke_{ObjectKind::Poke, 501.0f, 432.0f},
      menuButton_{ObjectKind::MenuButton, 1215.0f, 704.0f} {
    reset();
    executeMenuCommand(Mood::Smile);
}

void OuORuntime::reset() {
    for (float& value : config_) {
        value = 0.0f;
    }
    for (float& value : globals_) {
        value = 0.0f;
    }
    globals_[0] = lockEmotions_ ? 1.0f : 0.0f;
    globals_[2] = staticMood_ ? 1.0f : 0.0f;

    CtfObject* objects[] = {
        &eyeRight_, &eyeLeft_, &topLeft_, &bottomLeft_, &topRight_, &bottomRight_,
        &atLeft_, &atRight_, &vLeft_, &vRight_, &blushRight_, &blushLeft_,
        &sbLeft_, &sbRight_, &mouth_, &poke_, &menuButton_,
    };
    for (CtfObject* object : objects) {
        object->reset();
    }

    topLeft_.scaleX = topLeft_.scaleY = 2.0f;
    topRight_.scaleX = topRight_.scaleY = 2.0f;
    bottomLeft_.scaleX = bottomLeft_.scaleY = 2.0f;
    bottomRight_.scaleX = bottomRight_.scaleY = 2.0f;
    atLeft_.visible = false;
    atRight_.visible = false;
    vLeft_.visible = false;
    vRight_.visible = false;
    vLeft_.dir = 0;
    vRight_.dir = 16;
    poke_.visible = false;
    blushLeft_.visible = false;
    blushRight_.visible = false;
    blushLeft_.av[0] = 255.0f;
    blushRight_.av[0] = 255.0f;
    mouth_.visible = true;
    mouth_.av[0] = mouth_.dir;
    poke_.av[0] = 0.0f;
    poke_.av[1] = 1280.0f;
    poke_.av[2] = 1280.0f;
    poke_.av[3] = 1280.0f;
    poke_.av[4] = 0.0f;
    menuButton_.visible = !menuHidden_;
    menuButton_.av[1] = menuHidden_ ? 100000.0f : 0.0f;
    menuFrame_ = 3;
    idleSecondMs_ = 0.0f;
    lockedIdleMs_ = 0.0f;
    autoBlinkMs_ = 0.0f;
    autoMood600Ms_ = 0.0f;
    autoMood1000Ms_ = 0.0f;
    touchMovement250Ms_ = 0.0f;
    shakeMeter_ = 0.0f;
    lastAccelerationDelta_ = 0.0f;
    hasAcceleration_ = false;
    tiltWinkArmed_ = false;
    lastShakeMs_ = 0;
    lastTiltWinkMs_ = 0;
    shakeEvents_ = 0;
}

void OuORuntime::setRandomSeed(uint32_t seed) {
    rng_ = seed == 0 ? 0x12345678u : seed;
}

void OuORuntime::setMood(Mood mood) {
    mood_ = mood;
    executeMenuCommand(mood_);
}

void OuORuntime::setMenuHidden(bool hidden) {
    menuHidden_ = hidden;
    menuButton_.visible = !hidden;
    if (hidden) {
        menuButton_.av[1] = 100000.0f;
        menuFrame_ = 0;
    } else {
        menuButton_.av[1] = 0.0f;
        menuFrame_ = 3;
    }
}

void OuORuntime::setBlush(bool enabled) {
    persistentBlush_ = enabled;
    blushLeft_.av[0] = enabled ? 0.0f : 255.0f;
    blushRight_.av[0] = blushLeft_.av[0];
    blushLeft_.visible = enabled;
    blushRight_.visible = enabled;
}

void OuORuntime::setMouthStretchingEnabled(bool enabled) {
    mouthStretchingEnabled_ = enabled;
    if (!enabled) {
        hidePokeHelper(true);
        if (mouth_.dir == 7) {
            setDirection(mouth_, INT32_MIN);
            mouth_.scaleX = 1.5f;
            mouth_.scaleY = 0.8f;
        }
    }
}

void OuORuntime::setLockEmotions(bool enabled) {
    lockEmotions_ = enabled;
    globals_[0] = enabled ? 1.0f : 0.0f;
}

void OuORuntime::setStaticMood(bool enabled) {
    staticMood_ = enabled;
    globals_[2] = enabled ? 1.0f : 0.0f;
    if (enabled) {
        autoMood600Ms_ = 0.0f;
        autoMood1000Ms_ = 0.0f;
        if (!touching_ && !sleeping_) {
            executeMenuCommand(mood_);
        }
    }
}

void OuORuntime::executeMenuCommand(Mood command) {
    beginOriginalCommand(command);
    switch (command) {
        case Mood::Smile: runOriginalSmileEvent(); break;
        case Mood::Angry: runOriginalGrumpEvent(); break;
        case Mood::Upset: runOriginalUpsetEvent(); break;
        case Mood::Surprise: runOriginalSurpriseEvent(); break;
        case Mood::Blank: runOriginalBlankEvent(); break;
        case Mood::Sad: runOriginalSadEvent(); break;
        case Mood::Cheeky: runOriginalCheekyEvent(); break;
        case Mood::Frown: runOriginalFrownEvent(); break;
        case Mood::Squint: originalBlink(false); break;
        case Mood::Blink: originalBlink(true); break;
    }
}

void OuORuntime::beginOriginalCommand(Mood) {
    sleeping_ = false;
    config_[AV_TOP_GRUMP] = 0.0f;
    config_[AV_TOP_SAD] = 0.0f;
    config_[AV_BOTTOM_CHEEKY] = 0.0f;
    config_[AV_BOTTOM_FROWN] = 0.0f;
    topLeft_.av[0] = topRight_.av[0] = 0.0f;
    bottomLeft_.av[0] = bottomRight_.av[0] = 0.0f;
    topLeft_.angle = topRight_.angle = 0.0f;
    bottomLeft_.angle = bottomRight_.angle = 0.0f;
    setDirection(eyeLeft_, eyeRight_, 1);
    setAlterable(eyeLeft_, eyeRight_, 0, 0.0f);
    setAlterable(eyeLeft_, eyeRight_, 1, 0.0f);
    eyeLeft_.scaleX = eyeRight_.scaleX = 1.0f;
    eyeLeft_.scaleY = eyeRight_.scaleY = 1.0f;
    setScale(mouth_, 1.0f);
    vLeft_.visible = false;
    vRight_.visible = false;
    atLeft_.visible = false;
    atRight_.visible = false;
    hidePokeHelper(true);
    mouthFrame_ = 0;
    if (!persistentBlush_) {
        setAlterable(blushLeft_, blushRight_, 0, 255.0f);
        blushLeft_.visible = false;
        blushRight_.visible = false;
    }
}

void OuORuntime::originalBlink(bool blink2) {
    setDirection(eyeLeft_, eyeRight_, 2);
    setScaleY(eyeLeft_, eyeRight_, 3.0f);
    setAlterable(eyeLeft_, eyeRight_, 0, blink2 ? -90.0f : -60.0f);
    if (!blink2) {
        setAlterable(eyeLeft_, eyeRight_, 1, 200.0f);
        config_[AV_BOTTOM_FROWN] = 0.0f;
        config_[AV_BOTTOM_CHEEKY] = 0.0f;
    }
}

void OuORuntime::runOriginalSmileEvent() {
    setDirection(mouth_, 16);
    setDirection(eyeLeft_, eyeRight_, 2);
    setScaleY(eyeLeft_, eyeRight_, 3.0f);
    setAlterable(eyeLeft_, eyeRight_, 0, -6.0f);
    config_[AV_RESET_DELAY] = 80.0f + randomInt(60) * 4.0f;
    config_[AV_BOTTOM_CHEEKY] = config_[AV_RESET_DELAY];
    setAlterable(eyeLeft_, eyeRight_, 1, 170.0f);
    setAlterable(blushLeft_, blushRight_, 0, 0.0f);
    blushLeft_.visible = true;
    blushRight_.visible = true;
    config_[AV_BOTTOM_FROWN] = 0.0f;
}

void OuORuntime::runOriginalGrumpEvent() {
    config_[AV_TOP_GRUMP] = 120.0f + randomInt(10) * 20.0f;
    setDirection(mouth_, 8);
    config_[AV_TOP_SAD] = 0.0f;
}

void OuORuntime::runOriginalUpsetEvent() {
    setDirection(mouth_, 8);
}

void OuORuntime::runOriginalSurpriseEvent() {
    setDirection(mouth_, 64);
}

void OuORuntime::runOriginalBlankEvent() {
    setDirection(mouth_, 4);
    setScale(mouth_, 2.0f);
}

void OuORuntime::runOriginalSadEvent() {
    int bound = (int)fmaxf(1.0f, config_[AV_IDLE_TIME] / 2.0f);
    config_[AV_TOP_SAD] = 2000.0f + randomInt(bound) * 22.0f;
    setDirection(mouth_, 4);
    setScale(mouth_, 2.0f);
    config_[AV_BOTTOM_CHEEKY] = 0.0f;
    config_[AV_TOP_GRUMP] = 0.0f;
}

void OuORuntime::runOriginalCheekyEvent() {
    config_[AV_BOTTOM_CHEEKY] = 30.0f + randomInt(12) * 15.0f;
    config_[AV_BOTTOM_FROWN] = 0.0f;
}

void OuORuntime::runOriginalFrownEvent() {
    config_[AV_BOTTOM_FROWN] = 70.0f + randomInt(12) * 15.0f;
    config_[AV_BOTTOM_CHEEKY] = 0.0f;
    setAlterable(eyeLeft_, eyeRight_, 1, 0.0f);
}

void OuORuntime::tick() {
    globals_[7] += 1.0f;

    if (touching_) {
        float dxFromCache = touchDesignX_ - config_[8];
        float dyFromCache = touchDesignY_ - config_[9];
        bool every250 = advanceTouchMovement250();
        float movementScore = originalTouchMovementDistance(dxFromCache, dyFromCache);
        if (config_[AV_TOUCH_RESET] > 0.0f && config_[AV_TOUCH_COUNT] > 0.0f && every250 && movementScore > 6.0f) {
            config_[AV_TOUCH_COUNT] = fminf(30.0f, config_[AV_TOUCH_COUNT] + 1.0f);
        }
        if (config_[AV_TOUCH_RESET] > 0.0f && config_[AV_TOUCH_COUNT] < 4.0f && every250 && movementScore > 110.0f && !vLeft_.visible && !vRight_.visible) {
            runOriginalFastLatchSmileEvent();
        }
        config_[8] = touchDesignX_;
        config_[9] = touchDesignY_;
        config_[AV_IDLE_TIME] = 0.0f;
        idleSecondMs_ = 0.0f;
        lockedIdleMs_ = 0.0f;
        runOriginalTouchInteractions();
    } else {
        config_[8] = 0.0f;
        config_[9] = 0.0f;
        touchMovement250Ms_ = 0.0f;
        if (sleeping_) {
            config_[AV_IDLE_TIME] = 0.0f;
            idleSecondMs_ = 0.0f;
            lockedIdleMs_ = 0.0f;
        } else if (lockEmotions_) {
            idleSecondMs_ = 0.0f;
            lockedIdleMs_ += TickMs;
            while (lockedIdleMs_ >= 800.0f) {
                config_[AV_IDLE_TIME] += 1.0f;
                lockedIdleMs_ -= 800.0f;
            }
        } else {
            lockedIdleMs_ = 0.0f;
            config_[AV_IDLE_TIME] += 0.5f;
            idleSecondMs_ += TickMs;
            while (idleSecondMs_ >= 1000.0f) {
                config_[AV_IDLE_TIME] += 30.0f;
                idleSecondMs_ -= 1000.0f;
            }
        }
    }

    runOriginalVOverlayEvents();
    updateOriginalBlinkCounter();

    if (config_[AV_TOUCH_RESET] > 0.0f) {
        config_[AV_TOUCH_RESET] -= 1.0f;
        topLeft_.y += ((301.0f - 112.0f) - topLeft_.y) / 4.0f;
        topRight_.y += ((301.0f - 112.0f) - topRight_.y) / 4.0f;
        bottomLeft_.y += ((301.0f + 112.0f) - bottomLeft_.y) / 4.0f;
        bottomRight_.y += ((301.0f + 112.0f) - bottomRight_.y) / 4.0f;
        setAlterable(blushLeft_, blushRight_, 0, 255.0f);
        atLeft_.visible = false;
        atRight_.visible = false;
        if (!touching_) {
            setDirection(eyeLeft_, eyeRight_, 1);
            if (config_[AV_TOUCH_COUNT] > 13.0f) {
                setDirection(mouth_, 8);
                mouth_.scaleY = 1.5f;
            } else if (config_[AV_TOUCH_COUNT] <= 6.0f) {
                setDirection(mouth_, 2);
                mouth_.scaleY = 1.5f;
            }
        }
    } else {
        topLeft_.y += ((301.0f - 220.0f) - topLeft_.y) / 3.0f;
        topRight_.y += ((301.0f - 220.0f) - topRight_.y) / 3.0f;
        bottomLeft_.y += ((301.0f + 208.0f) - bottomLeft_.y) / 3.0f;
        bottomRight_.y += ((301.0f + 208.0f) - bottomRight_.y) / 3.0f;
        if (!touching_) {
            vLeft_.visible = false;
            vRight_.visible = false;
        }
    }

    bool resetDelayReachedZero = false;
    if (config_[AV_RESET_DELAY] > 0.0f) {
        config_[AV_RESET_DELAY] -= 1.0f;
        resetDelayReachedZero = config_[AV_RESET_DELAY] <= 0.0f;
    } else {
        if (!persistentBlush_) {
            setAlterable(blushLeft_, blushRight_, 0, 255.0f);
        }
        atLeft_.visible = false;
        atRight_.visible = false;
    }
    if (resetDelayReachedZero && !touching_ && !sleeping_ && mouth_.dir != 7 && mouth_.dir != 31) {
        setDirection(mouth_, config_[AV_TOUCH_COUNT] <= 13.0f ? 1 : 4);
    }

    maybeRunOriginalLatchReleaseRandomEvent();

    if (config_[AV_SHAKE_COOLDOWN] > 0.0f) {
        config_[AV_SHAKE_COOLDOWN] -= 1.0f;
    }

    runOriginalIdleRandomMoodEvents();

    if (globals_[0] == 0.0f) {
        if (config_[AV_TOP_GRUMP] > 0.0f) {
            config_[AV_TOP_GRUMP] -= 1.0f;
        }
        if (config_[AV_TOP_SAD] > 0.0f) {
            config_[AV_RESET_DELAY] = 120.0f;
            config_[AV_TOP_SAD] -= 1.0f;
        }
        if (config_[AV_BOTTOM_CHEEKY] > 0.0f) {
            config_[AV_BOTTOM_CHEEKY] -= 1.0f;
        }
        if (config_[AV_BOTTOM_FROWN] > 0.0f) {
            config_[AV_BOTTOM_FROWN] -= 1.0f;
        }
    }

    if (config_[AV_TOP_GRUMP] > 0.0f) {
        topLeft_.av[0] += (-66.0f - topLeft_.av[0]) / 8.0f;
        topRight_.av[0] += (-66.0f - topRight_.av[0]) / 8.0f;
        topLeft_.y += ((301.0f - 80.0f) - topLeft_.y) / 6.0f;
        topRight_.y += ((301.0f - 80.0f) - topRight_.y) / 6.0f;
    }
    if (config_[AV_TOP_SAD] > 0.0f) {
        topLeft_.av[0] += (66.0f - topLeft_.av[0]) / 8.0f;
        topRight_.av[0] += (66.0f - topRight_.av[0]) / 8.0f;
        topLeft_.y += ((301.0f - 40.0f) - topLeft_.y) / 6.0f;
        topRight_.y += ((301.0f - 40.0f) - topRight_.y) / 6.0f;
        bottomLeft_.av[0] += (-20.0f - bottomLeft_.av[0]) / 8.0f;
        bottomRight_.av[0] += (-20.0f - bottomRight_.av[0]) / 8.0f;
    }
    if (config_[AV_BOTTOM_CHEEKY] > 0.0f) {
        bottomLeft_.y += (400.0f - bottomLeft_.y) / 3.0f;
        bottomRight_.y += (400.0f - bottomRight_.y) / 3.0f;
    }
    if (config_[AV_BOTTOM_FROWN] > 0.0f) {
        topLeft_.y += (380.0f - topLeft_.y) / 8.0f;
        topRight_.y += (380.0f - topRight_.y) / 8.0f;
    }

    updateOriginalAutoBlink();
    updateOriginalBlushPositions();

    topLeft_.av[0] += -topLeft_.av[0] / 8.0f;
    topRight_.av[0] += -topRight_.av[0] / 8.0f;
    bottomLeft_.av[0] += -bottomLeft_.av[0] / 8.0f;
    bottomRight_.av[0] += -bottomRight_.av[0] / 8.0f;
    if (shouldRunOriginalGlobalAngleGate()) {
        topLeft_.angle = topLeft_.av[0];
        topRight_.angle = -topRight_.av[0];
        bottomLeft_.angle = bottomLeft_.av[0];
        bottomRight_.angle = -bottomRight_.av[0];
    }

    eyeLeft_.scaleY += (1.0f - eyeLeft_.scaleY) / 2.0f;
    eyeLeft_.scaleX += (1.0f - eyeLeft_.scaleX) / 2.0f;
    eyeRight_.scaleY += (1.0f - eyeRight_.scaleY) / 2.0f;
    eyeRight_.scaleX += (1.0f - eyeRight_.scaleX) / 2.0f;
    applyOriginalEyeAngles();

    runOriginalMouthStretchCollapse();
    runOriginalMouthDirectionCache();
    runOriginalMouthNormalScaleReturn();

    vLeft_.scaleX += (1.0f - vLeft_.scaleX) / 3.0f;
    vLeft_.scaleY += (1.0f - vLeft_.scaleY) / 3.0f;
    vRight_.scaleX = vLeft_.scaleX;
    vRight_.scaleY = vLeft_.scaleY;
    atLeft_.scaleX += (1.0f - atLeft_.scaleX) / 6.0f;
    atLeft_.scaleY = atLeft_.scaleX;
    atRight_.scaleX = atLeft_.scaleX;
    atRight_.scaleY = atLeft_.scaleY;
    if (atLeft_.visible) {
        atLeft_.angle += 10.0f;
        atRight_.angle = atLeft_.angle;
    }
    sbLeft_.y += (560.0f - sbLeft_.y) / 8.0f;
    sbRight_.y = sbLeft_.y;

    updatePokeAnimationFrame();
    runOriginalMenuButtonEvents();

    blushLeft_.visible = blushLeft_.av[0] < 240.0f || persistentBlush_;
    blushRight_.visible = blushLeft_.visible;
}

void OuORuntime::touchDown(float screenX, float screenY) {
    touching_ = true;
    downX_ = screenX;
    downY_ = screenY;
    touchDesignX_ = screenToDesignX(screenX);
    touchDesignY_ = screenToDesignY(screenY);
    downDesignX_ = touchDesignX_;
    downDesignY_ = touchDesignY_;
    downIdleTime_ = config_[AV_IDLE_TIME];
    downTopSad_ = config_[AV_TOP_SAD];
    config_[8] = touchDesignX_;
    config_[9] = touchDesignY_;
    updateOriginalTouchLatch();
    maybeStartOriginalMouthStretch();
}

void OuORuntime::touchMove(float screenX, float screenY) {
    touchDesignX_ = screenToDesignX(screenX);
    touchDesignY_ = screenToDesignY(screenY);
    updateOriginalTouchLatch();
}

void OuORuntime::touchUp(float screenX, float screenY) {
    touchDesignX_ = screenToDesignX(screenX);
    touchDesignY_ = screenToDesignY(screenY);
    touching_ = false;
    maybeCollapsePokeHelper();
    hidePokeHelper(true);
    handleOriginalClick(screenX, screenY);
}

void OuORuntime::touchCancel() {
    touching_ = false;
    hidePokeHelper(true);
}

void OuORuntime::setAcceleration(float x, float y, float z, uint32_t nowMs) {
    if (hasAcceleration_) {
        float delta = fabsf(x - lastAcceleration_[0]) + fabsf(y - lastAcceleration_[1]) + fabsf(z - lastAcceleration_[2]);
        lastAccelerationDelta_ = delta;
        constexpr float IdleNudgeDeltaG = 0.04f;
        constexpr float ShakeTriggerDeltaG = 0.30f;
        constexpr float ShakeDecayDeltaG = 0.10f;
        if (delta > IdleNudgeDeltaG && config_[AV_IDLE_TIME] > 100.0f && config_[AV_IDLE_TIME] < 680.0f) {
            config_[AV_IDLE_TIME] = 100.0f;
        }
        if (delta > ShakeTriggerDeltaG) {
            shakeMeter_ += 2.0f;
        } else if (delta <= ShakeDecayDeltaG && shakeMeter_ > 0.0f) {
            shakeMeter_ -= 1.0f;
        }
        maybeRunOriginalShakeEvent(nowMs);
        maybeRunOriginalSideWink(x, nowMs);
    } else {
        hasAcceleration_ = true;
        lastAccelerationDelta_ = 0.0f;
    }
    lastAcceleration_[0] = x;
    lastAcceleration_[1] = y;
    lastAcceleration_[2] = z;
}

void OuORuntime::handleOriginalClick(float screenX, float screenY) {
    if (isMenuButtonTap(screenX, screenY) && isMenuButtonTap(downX_, downY_)) {
        return;
    }
    if (fabsf(screenX - downX_) > 8.0f || fabsf(screenY - downY_) > 8.0f) {
        return;
    }
    maybeRunOriginalLongSadClickMode();
    if (inZone(550.0f, 360.0f, 730.0f, 480.0f)) {
        config_[AV_BOTTOM_FROWN] = 0.0f;
        config_[AV_BOTTOM_CHEEKY] = 0.0f;
        setDirection(mouth_, 32);
        config_[AV_RESET_DELAY] = 200.0f;
        setAlterable(eyeLeft_, eyeRight_, 0, -100.0f);
        clearPokeFlags();
        return;
    }
    if (mouthStretchingEnabled_ && mouth_.dir != 7 && inZone(520.0f, 470.0f, 760.0f, 570.0f)) {
        startOriginalMouthStretch();
        return;
    }
    if (mouth_.dir == 7 && inZone(360.0f, 710.0f, 920.0f, 900.0f)) {
        showOriginalVOverlay(1.5f);
        return;
    }
    if (isEyeZone(touchDesignX_, touchDesignY_)) {
        originalEyeClick();
    }
}

void OuORuntime::maybeRunOriginalLongSadClickMode() {
    if (downIdleTime_ <= 700.0f || downTopSad_ <= 0.0f) {
        return;
    }
    setDirection(mouth_, 16);
    config_[AV_TOP_SAD] = 0.0f;
    config_[AV_BOTTOM_CHEEKY] = 0.0f;
    config_[AV_BOTTOM_FROWN] = 0.0f;
    config_[AV_TOP_GRUMP] = 0.0f;
    setDirection(eyeLeft_, eyeRight_, 2);
    setAlterable(eyeLeft_, eyeRight_, 0, 1.0f);
    config_[AV_SHAKE_COOLDOWN] = 120.0f;
    config_[AV_IDLE_TIME] = 0.0f;
}

void OuORuntime::runOriginalTouchInteractions() {
    updateOriginalLidDrag();
    if (mouthStretchingEnabled_ && mouth_.dir == 7) {
        updateOriginalMouthStretch();
        return;
    }
    if (mouth_.dir != 7) {
        updateOriginalPokeHelper();
    } else {
        hidePokeHelper(true);
    }
}

void OuORuntime::maybeStartOriginalMouthStretch() {
    if (mouthStretchingEnabled_ && mouth_.dir != 7 && inZone(520.0f, 470.0f, 760.0f, 570.0f)) {
        startOriginalMouthStretch();
    }
}

void OuORuntime::startOriginalMouthStretch() {
    int previousMouthDirection = (int)lroundf(mouth_.av[0]);
    setDirection(mouth_, 128);
    mouth_.scaleY = 0.1f;
    selectMouthStretchFrame(previousMouthDirection);
}

void OuORuntime::updateOriginalLidDrag() {
    if (config_[AV_TOUCH_RESET] != 0.0f || poke_.visible) {
        return;
    }
    bool topDragged = false;
    if (fabsf(touchDesignX_ - DesignW * 0.5f) > 280.0f) {
        if (touchOverObject(topLeft_, 34)) {
            applyOriginalTopLidDrag(topLeft_);
            topDragged = true;
        }
        if (touchOverObject(topRight_, 34)) {
            applyOriginalTopLidDrag(topRight_);
            topDragged = true;
        }
    }
    if (topDragged && randomInt(64) == 0) {
        float centerDistance = fabsf(touchDesignX_ - DesignW * 0.5f);
        if (centerDistance > 280.0f && centerDistance < 380.0f) {
            config_[AV_TOP_GRUMP] = fmaxf(config_[AV_TOP_GRUMP], 420.0f);
        } else if (centerDistance >= 380.0f && centerDistance <= 460.0f) {
            config_[AV_BOTTOM_FROWN] = fmaxf(config_[AV_BOTTOM_FROWN], 420.0f);
            config_[AV_TOP_SAD] = 0.0f;
        } else if (centerDistance > 460.0f) {
            config_[AV_TOP_SAD] = fmaxf(config_[AV_TOP_SAD], 420.0f);
            config_[AV_BOTTOM_FROWN] = 0.0f;
        }
    }
    bool bottomDragged = false;
    if (touchOverObject(bottomLeft_, 34)) {
        applyOriginalBottomLidDrag(bottomLeft_);
        bottomDragged = true;
    }
    if (touchOverObject(bottomRight_, 34)) {
        applyOriginalBottomLidDrag(bottomRight_);
        bottomDragged = true;
    }
    if (bottomDragged && randomInt(64) == 0) {
        config_[AV_BOTTOM_CHEEKY] = fmaxf(config_[AV_BOTTOM_CHEEKY], 420.0f);
    }
}

void OuORuntime::applyOriginalTopLidDrag(CtfObject& lid) {
    lid.y += (fminf(300.0f, touchDesignY_ + 16.0f) - lid.y) / 2.0f;
    float side = sideFromFrameCenter(touchDesignX_);
    float localAngle = clamp((touchDesignX_ - lid.x) / 5.0f, -30.0f, 30.0f);
    lid.av[0] = side * localAngle;
}

void OuORuntime::applyOriginalBottomLidDrag(CtfObject& lid) {
    lid.y += (fmaxf(340.0f, touchDesignY_) - lid.y) / 2.0f;
    float side = sideFromFrameCenter(touchDesignX_);
    float localAngle = clamp((touchDesignX_ - lid.x) / 4.0f, -20.0f, 20.0f);
    lid.av[0] = side * localAngle * -1.0f;
}

void OuORuntime::updateOriginalMouthStretch() {
    if (!inZone(360.0f, 480.0f, 920.0f, 900.0f)) {
        return;
    }
    config_[AV_RESET_DELAY] = 60.0f;
    mouth_.av[0] = mouth_.dir;
    float targetY = (fminf(800.0f, touchDesignY_) * 10.5f / DesignH) - 6.5f;
    mouth_.scaleY = fminf(2.6f, mouth_.scaleY + (targetY - mouth_.scaleY) / 4.0f);
    mouth_.scaleX = 0.3f + fabsf(DesignW * 0.5f - touchDesignX_) / (DesignW / 14.0f);
    sbLeft_.y += (380.0f - sbLeft_.y) / 8.0f;
    sbRight_.y = sbLeft_.y;
    if (inZone(360.0f, 710.0f, 920.0f, 900.0f)) {
        if (!vLeft_.visible || !vRight_.visible) {
            showOriginalVOverlay(1.5f);
        }
        if (vLeft_.visible && vRight_.visible) {
            mouth_.scaleX += originalStretchJitter();
            mouth_.scaleY += originalStretchJitter();
        }
    }
}

float OuORuntime::originalStretchJitter() {
    return -0.1f + randomInt(10) / 50.0f;
}

void OuORuntime::updateOriginalPokeHelper() {
    if (touchDesignY_ <= 380.0f || touchDesignY_ >= 700.0f) {
        return;
    }
    float deltaFromCenter = DesignW * 0.5f - touchDesignX_;
    float absDx = fabsf(deltaFromCenter);
    if (absDx < 300.0f && absDx > 100.0f) {
        poke_.visible = true;
        poke_.flags[FLAG_POKE_ACTIVE] = true;
    }
    if (!poke_.visible) {
        mouth_.visible = true;
        return;
    }
    mouth_.visible = false;
    poke_.x = DesignW * 0.5f - deltaFromCenter / 6.0f;
    if (absDx <= 100.0f) {
        poke_.flags[FLAG_POKE_LEFT] = false;
        poke_.flags[FLAG_POKE_RIGHT] = false;
        if (poke_.av[4] != 0.0f) {
            setDirectionDirect(poke_, 8 + (int)lroundf(poke_.av[4]));
            poke_.av[3] = (poke_.av[1] + poke_.av[2]) / 2.0f;
            poke_.x = DesignW * 0.5f;
        }
    } else if (deltaFromCenter < 0.0f) {
        poke_.flags[FLAG_POKE_LEFT] = true;
        poke_.av[2] = fminf(seedValue(poke_.av[2]), absDx);
        poke_.av[3] = poke_.av[2];
        poke_.av[4] = -1.0f;
        setDirection(poke_, 65536);
        sbLeft_.y += (360.0f - sbLeft_.y) / 8.0f;
        sbRight_.y = sbLeft_.y;
    } else {
        poke_.flags[FLAG_POKE_RIGHT] = true;
        poke_.av[1] = fminf(seedValue(poke_.av[1]), absDx);
        poke_.av[3] = poke_.av[1];
        poke_.av[4] = 1.0f;
        setDirection(poke_, 1);
        sbLeft_.y += (360.0f - sbLeft_.y) / 8.0f;
        sbRight_.y = sbLeft_.y;
    }
    if (poke_.flags[FLAG_POKE_LEFT] && poke_.flags[FLAG_POKE_RIGHT]) {
        hidePokeHelper(false);
        return;
    }
    poke_.av[0] = fminf(7.0f, fmaxf(0.0f, 9.0f - poke_.av[3] / 40.0f));
}

float OuORuntime::seedValue(float value) const {
    return value <= 0.0f ? 1280.0f : value;
}

void OuORuntime::runOriginalMouthStretchCollapse() {
    if (mouth_.dir == 7) {
        mouth_.scaleY += (0.1f - mouth_.scaleY) * 0.3f;
        mouth_.scaleX += (0.9f - mouth_.scaleX) * 0.5f;
        if (mouth_.scaleY <= 0.12f) {
            setDirection(mouth_, INT32_MIN);
            mouth_.scaleX = 1.5f;
            vLeft_.visible = false;
            vRight_.visible = false;
        }
    } else {
        mouthFrame_ = 0;
    }
}

void OuORuntime::runOriginalMouthDirectionCache() {
    if (mouth_.av[0] == mouth_.dir) {
        return;
    }
    if (mouth_.scaleY >= 0.9f && mouth_.scaleY <= 1.1f) {
        mouth_.scaleY = 0.4f;
    }
    mouth_.av[0] = mouth_.dir;
}

void OuORuntime::runOriginalMouthNormalScaleReturn() {
    if (mouth_.dir == 7) {
        return;
    }
    mouth_.scaleY += (1.0f - mouth_.scaleY) / 3.0f;
    mouth_.scaleX += (1.0f - mouth_.scaleX) / 3.0f;
}

void OuORuntime::runOriginalMenuButtonEvents() {
    if (menuButton_.av[1] > 0.0f && menuButton_.av[1] < 100000.0f) {
        menuButton_.av[1] -= 1.0f;
        menuButton_.av[0] += 8.0f;
    }
    menuButton_.av[0] = fminf(220.0f + menuButton_.av[1], menuButton_.av[0] + 6.0f);
    if (menuButton_.av[0] > 0.0f) {
        menuButton_.visible = !menuHidden_;
        menuFrame_ = 3;
    }
    if (menuButton_.av[0] > 90.0f) {
        menuFrame_ = 2;
    }
    if (menuButton_.av[0] > 145.0f) {
        menuFrame_ = 1;
    }
    if (menuButton_.av[0] > 200.0f) {
        menuFrame_ = 0;
    }
    if (menuButton_.av[0] > 220.0f) {
        menuButton_.visible = false;
    }
}

void OuORuntime::updatePokeAnimationFrame() {
    if (!poke_.visible) {
        mouth_.visible = true;
        poke_.av[0] = 0.0f;
        return;
    }
    mouth_.visible = false;
    int target = (int)lroundf(clamp(poke_.av[0], 0.0f, 7.0f));
    if (pokeFrame_ < target) {
        pokeFrame_++;
    } else if (pokeFrame_ > target) {
        pokeFrame_--;
    }
}

void OuORuntime::hidePokeHelper(bool resetSeeds) {
    poke_.visible = false;
    mouth_.visible = true;
    pokeFrame_ = 0;
    poke_.av[0] = 0.0f;
    if (resetSeeds) {
        poke_.av[1] = 1280.0f;
        poke_.av[2] = 1280.0f;
        poke_.av[3] = 1280.0f;
        poke_.av[4] = 0.0f;
    }
    clearPokeFlags();
}

void OuORuntime::maybeCollapsePokeHelper() {
    if (!poke_.visible || (!poke_.flags[FLAG_POKE_LEFT] && !poke_.flags[FLAG_POKE_RIGHT])) {
        return;
    }
    mouth_.scaleX = 2.0f;
    mouth_.scaleY = 0.8f;
    setDirection(mouth_, INT32_MIN);
    config_[AV_RESET_DELAY] = 60.0f;
}

void OuORuntime::clearPokeFlags() {
    for (bool& flag : poke_.flags) {
        flag = false;
    }
}

void OuORuntime::updateOriginalBlushPositions() {
    float leftY = bottomLeft_.y * 0.8f + bottomLeft_.av[0] * 1.1f;
    float rightY = bottomRight_.y * 0.8f + bottomRight_.av[0] * 1.1f;
    blushLeft_.y += (leftY - blushLeft_.y) / 2.0f;
    blushRight_.y += (rightY - blushRight_.y) / 2.0f;
}

void OuORuntime::updateOriginalTouchLatch() {
    if (touching_ && touchDesignX_ > 440.0f && touchDesignX_ < 840.0f && touchDesignY_ < 300.0f) {
        config_[AV_TOUCH_RESET] = 50.0f;
        config_[AV_RESET_DELAY] = -1.0f;
        config_[AV_TOP_GRUMP] = 0.0f;
        config_[AV_TOP_SAD] = 0.0f;
        config_[AV_BOTTOM_CHEEKY] = 0.0f;
        config_[AV_BOTTOM_FROWN] = 0.0f;
        config_[AV_SHAKE_COOLDOWN] = 120.0f;
    }
}

void OuORuntime::updateOriginalBlinkCounter() {
    updateOriginalBlinkCounter(eyeLeft_, topLeft_, bottomLeft_);
    updateOriginalBlinkCounter(eyeRight_, topRight_, bottomRight_);
}

void OuORuntime::updateOriginalBlinkCounter(CtfObject& eye, CtfObject& topLid, CtfObject& bottomLid) {
    if (eye.dir == 0 || sleeping_) {
        return;
    }
    eye.av[0] += 1.0f;
    topLid.y += (250.0f - topLid.y) / 3.0f;
    bottomLid.y += (301.0f - bottomLid.y) / 6.0f;
    if (eye.av[0] > 3.0f) {
        setDirection(eye, 1);
        eye.av[0] = 0.0f;
        eye.scaleY = 0.75f;
        eye.angle = 0.0f;
    }
}

void OuORuntime::updateOriginalAutoBlink() {
    autoBlinkMs_ += TickMs;
    if (autoBlinkMs_ < 80.0f || eyeLeft_.dir != 0 || config_[AV_TOUCH_RESET] > 0.0f) {
        return;
    }
    autoBlinkMs_ = 0.0f;
    int chance = 16 + (int)lroundf(fmaxf(0.0f, blushLeft_.av[0]) / 8.0f);
    if (chance > 0 && randomInt(chance) == 0) {
        setDirection(eyeLeft_, eyeRight_, 2);
        setScaleY(eyeLeft_, eyeRight_, 3.0f);
    }
}

void OuORuntime::runOriginalIdleRandomMoodEvents() {
    if (staticMood_) {
        autoMood600Ms_ = 0.0f;
        autoMood1000Ms_ = 0.0f;
        return;
    }
    autoMood600Ms_ += TickMs;
    autoMood1000Ms_ += TickMs;
    bool every600 = false;
    while (autoMood600Ms_ >= 600.0f) {
        autoMood600Ms_ -= 600.0f;
        every600 = true;
    }
    bool every1000 = false;
    while (autoMood1000Ms_ >= 1000.0f) {
        autoMood1000Ms_ -= 1000.0f;
        every1000 = true;
    }
    if (every600 && config_[AV_IDLE_TIME] > 60.0f && config_[AV_IDLE_TIME] < 200.0f
            && config_[AV_TOUCH_RESET] == 0.0f && config_[AV_TOUCH_COUNT] < 3.0f && randomZero(40)) {
        runOriginalSmileEvent();
    }
    if (every1000 && config_[AV_IDLE_TIME] > 600.0f && randomZero(idleMoodChance(4.0f))) {
        runOriginalGrumpEvent();
    }
    if (every1000 && config_[AV_IDLE_TIME] > 450.0f && config_[AV_TOUCH_COUNT] > 2.0f && randomZero(idleMoodChance(3.0f))) {
        runOriginalUpsetEvent();
    }
    if (every1000 && config_[AV_IDLE_TIME] > 450.0f && config_[AV_TOUCH_COUNT] > 2.0f && randomZero(idleMoodChance(3.0f))) {
        runOriginalSurpriseEvent();
    }
    if (every1000 && config_[AV_IDLE_TIME] > 240.0f && config_[AV_TOUCH_COUNT] > 3.0f && randomZero(32)) {
        runOriginalBlankEvent();
    }
    if (every1000 && config_[AV_IDLE_TIME] > 730.0f && randomZero(8)) {
        runOriginalSadEvent();
    }
    if (every1000 && config_[AV_IDLE_TIME] > 150.0f && config_[AV_TOP_SAD] == 0.0f && randomZero(40)) {
        runOriginalCheekyEvent();
    }
    if (every600 && config_[AV_IDLE_TIME] > 300.0f) {
        int target = (int)fminf((3.0f - config_[AV_TOUCH_COUNT]), 0.0f);
        if (randomInt(20) == target) {
            runOriginalFrownEvent();
        }
    }
}

int OuORuntime::idleMoodChance(float pressureFactor) const {
    int chance = (int)(80.0f - config_[AV_TOUCH_COUNT] * pressureFactor);
    return chance < 16 ? 16 : chance;
}

bool OuORuntime::randomZero(int bound) {
    return bound > 0 && randomInt(bound) == 0;
}

float OuORuntime::originalTouchMovementDistance(float dx, float dy) const {
    return sqrtf(dx * dx + dy * dy);
}

bool OuORuntime::advanceTouchMovement250() {
    touchMovement250Ms_ += TickMs;
    bool reached = false;
    while (touchMovement250Ms_ >= 250.0f) {
        touchMovement250Ms_ -= 250.0f;
        reached = true;
    }
    return reached;
}

void OuORuntime::runOriginalFastLatchSmileEvent() {
    setDirection(mouth_, 16);
    setDirection(eyeLeft_, eyeRight_, 2);
    setAlterable(eyeLeft_, eyeRight_, 0, -10.0f);
    setAlterable(eyeLeft_, eyeRight_, 1, 200.0f);
    showOriginalVOverlay(1.5f);
}

void OuORuntime::maybeRunOriginalLatchReleaseRandomEvent() {
    if (config_[AV_TOUCH_RESET] != 1.0f || config_[AV_TOUCH_COUNT] > 1.0f || !randomZero(3)) {
        return;
    }
    runOriginalLatchReleaseRandomEvent((vLeft_.visible || vRight_.visible) ? 2 : 16);
}

void OuORuntime::runOriginalLatchReleaseRandomEvent(int mouthMask) {
    setDirection(mouth_, mouthMask);
    setDirection(eyeLeft_, eyeRight_, 2);
    setScaleY(eyeLeft_, eyeRight_, 3.0f);
    setAlterable(eyeLeft_, eyeRight_, 0, -10.0f);
    config_[AV_RESET_DELAY] = 180.0f + randomInt(60) * 5.0f;
    config_[AV_TOUCH_RESET] = 0.0f;
    setAlterable(eyeLeft_, eyeRight_, 1, 200.0f);
    setAlterable(blushLeft_, blushRight_, 0, 0.0f);
    blushLeft_.visible = true;
    blushRight_.visible = true;
    config_[AV_TOP_SAD] = (1.0f - fminf((float)randomInt(20), 1.0f)) * 500.0f;
    config_[AV_BOTTOM_CHEEKY] = config_[AV_RESET_DELAY] * (1.0f - fminf(1.0f, config_[AV_TOP_SAD]));
}

void OuORuntime::applyOriginalEyeAngles() {
    eyeLeft_.angle = 0.0f;
    eyeRight_.angle = 0.0f;
    applyOriginalEyeAngle(eyeLeft_);
    applyOriginalEyeAngle(eyeRight_);
}

void OuORuntime::applyOriginalEyeAngle(CtfObject& eye) {
    if (eye.dir == 0 || fabsf(eye.av[1]) <= 0.01f) {
        return;
    }
    float delta = eye.x - DesignW * 0.5f;
    if (fabsf(delta) < 1.0f) {
        return;
    }
    eye.angle = (delta / fabsf(delta)) * eye.av[1];
}

void OuORuntime::runOriginalVOverlayEvents() {
    if (vRight_.x > DesignW * 0.5f && vRight_.dir != 16) {
        setDirection(vRight_, 65536);
    }
    if ((vLeft_.visible || vRight_.visible) && !sleeping_) {
        setDirection(eyeLeft_, eyeRight_, 2);
        setAlterable(eyeLeft_, eyeRight_, 0, 1.0f);
    }
}

void OuORuntime::showOriginalVOverlay(float scaleY) {
    vLeft_.dir = 0;
    vRight_.dir = 16;
    vLeft_.visible = true;
    vRight_.visible = true;
    vLeft_.scaleY = scaleY;
    vRight_.scaleY = scaleY;
}

void OuORuntime::maybeRunOriginalShakeEvent(uint32_t nowMs) {
    if (shakeMeter_ <= 15.0f || config_[AV_TOUCH_RESET] != 0.0f || config_[AV_SHAKE_COOLDOWN] != 0.0f || touching_) {
        return;
    }
    if (lastShakeMs_ != 0 && nowMs - lastShakeMs_ < 500u) {
        return;
    }
    setDirection(mouth_, config_[AV_TOUCH_COUNT] < 4.0f ? 16 : 64);
    if (!atLeft_.visible && !atRight_.visible) {
        showOriginalAtOverlay();
    }
    config_[AV_RESET_DELAY] = fmaxf(config_[AV_RESET_DELAY], 120.0f + randomInt(15) * 4.0f);
    config_[AV_SHAKE_COOLDOWN] = 120.0f;
    shakeMeter_ = 0.0f;
    lastShakeMs_ = nowMs;
    shakeEvents_++;
}

void OuORuntime::showOriginalAtOverlay() {
    atLeft_.visible = true;
    atRight_.visible = true;
    atLeft_.x = eyeLeft_.x;
    atLeft_.y = eyeLeft_.y;
    atRight_.x = eyeRight_.x;
    atRight_.y = eyeRight_.y;
    setScale(atLeft_, 0.5f);
    setScale(atRight_, 0.5f);
}

void OuORuntime::maybeRunOriginalSideWink(float accelerometerX, uint32_t nowMs) {
    constexpr float RearmThresholdG = 0.25f;
    constexpr float TriggerThresholdG = 0.68f;
    constexpr uint32_t CooldownMs = 1100u;

    if (fabsf(accelerometerX) < RearmThresholdG) {
        tiltWinkArmed_ = true;
    }
    if (!tiltWinkingEnabled_) {
        return;
    }
    if (touching_ || config_[AV_TOUCH_RESET] != 0.0f || config_[AV_RESET_DELAY] > 0.0f) {
        return;
    }
    if (!tiltWinkArmed_ || fabsf(accelerometerX) < TriggerThresholdG) {
        return;
    }
    if (lastTiltWinkMs_ != 0 && nowMs - lastTiltWinkMs_ < CooldownMs) {
        return;
    }

    tiltWinkArmed_ = false;
    lastTiltWinkMs_ = nowMs;
    if (accelerometerX <= -TriggerThresholdG) {
        runOriginalSideWink(eyeLeft_);
    } else {
        runOriginalSideWink(eyeRight_);
    }
}

void OuORuntime::runOriginalSideWink(CtfObject& eye) {
    eye.scaleY = 3.0f;
    eye.av[1] = 200.0f;
    config_[AV_BOTTOM_CHEEKY] = 0.0f;
    config_[AV_BOTTOM_FROWN] = 0.0f;
    setDirection(eye, 4);
    eye.av[0] = fminf(-4.0f, eye.av[0]);
}

void OuORuntime::originalEyeClick() {
    if (config_[AV_TOUCH_RESET] != 0.0f) {
        return;
    }
    config_[AV_TOUCH_COUNT] += 1.0f;
    config_[AV_RESET_DELAY] = 130.0f;
    config_[AV_BOTTOM_CHEEKY] = 0.0f;
    config_[AV_BOTTOM_FROWN] = 0.0f;
    setDirection(eyeLeft_, eyeRight_, 4);
    setAlterable(eyeLeft_, eyeRight_, 0, -50.0f);
    setAlterable(eyeLeft_, eyeRight_, 1, 200.0f);
    setScaleY(eyeLeft_, eyeRight_, 3.0f);
    setAlterable(blushLeft_, blushRight_, 0, 255.0f);
    setDirection(mouth_, 8);

    if (config_[AV_TOUCH_COUNT] > 4.0f && config_[AV_TOUCH_COUNT] <= 10.0f) {
        setDirection(mouth_, 4);
    } else if (config_[AV_TOUCH_COUNT] > 10.0f && config_[AV_TOUCH_COUNT] <= 20.0f) {
        setDirection(mouth_, 8);
        config_[AV_TOP_GRUMP] = 120.0f + randomInt(10) * 20.0f;
        setAlterable(eyeLeft_, eyeRight_, 0, 0.0f);
        setAlterable(eyeLeft_, eyeRight_, 1, 20.0f);
    } else if (config_[AV_TOUCH_COUNT] > 20.0f) {
        config_[AV_BOTTOM_FROWN] = fminf(240.0f, config_[AV_BOTTOM_FROWN] + 80.0f);
        config_[AV_TOP_GRUMP] = 0.0f;
        setAlterable(eyeLeft_, eyeRight_, 0, 0.0f);
        setAlterable(eyeLeft_, eyeRight_, 1, 0.0f);
        setDirection(mouth_, 4);
    }
}

void OuORuntime::selectMouthStretchFrame(int previousDirection) {
    if (previousDirection == 7 || previousDirection == 31) {
        mouthFrame_ = randomInt(4);
    } else if (previousDirection >= 2 && previousDirection <= 3) {
        mouthFrame_ = 1;
    } else if (previousDirection == 5) {
        mouthFrame_ = 2;
    } else {
        mouthFrame_ = 0;
    }
    if (config_[AV_TOUCH_COUNT] > 9.0f) {
        mouthFrame_ = 1;
    }
}

void OuORuntime::buildDrawList(DrawList& out) const {
    out.clear();
    bool leftVVisible = vLeft_.visible;
    bool rightVVisible = vRight_.visible;
    if (!rightVVisible && eyeRight_.dir == 0) {
        addObject(out, eyeRight_);
    }
    if (!leftVVisible) {
        if (eyeLeft_.dir == 0) {
            addObject(out, eyeLeft_);
        }
        addObject(out, topLeft_);
        addObject(out, bottomLeft_);
    }
    if (!rightVVisible) {
        addObject(out, topRight_);
        addObject(out, bottomRight_);
        if (eyeRight_.dir != 0) {
            addObject(out, eyeRight_);
        }
    }
    if (!leftVVisible && eyeLeft_.dir != 0) {
        addObject(out, eyeLeft_);
    }
    addObject(out, atLeft_);
    addObject(out, atRight_);
    addObject(out, vLeft_);
    addObject(out, vRight_);
    addObject(out, blushRight_);
    addObject(out, blushLeft_);
    addObject(out, sbLeft_);
    addObject(out, sbRight_);
    addObject(out, mouth_);
    addObject(out, poke_);
    addObject(out, menuButton_);
}

void OuORuntime::addObject(DrawList& out, const CtfObject& object) const {
    if (!object.visible) {
        return;
    }
    uint16_t image = imageFor(object);
    if (image == 0) {
        return;
    }
    uint8_t alpha = 255;
    if (object.kind == ObjectKind::Blush) {
        alpha = (uint8_t)clamp(255.0f - object.av[0], 0.0f, 255.0f);
    }
    out.add({object.kind, image, object.x, object.y, object.scaleX, object.scaleY, object.angle, alpha});
}

uint16_t OuORuntime::imageFor(const CtfObject& object) const {
    switch (object.kind) {
        case ObjectKind::Eye:
            return object.dir == 0 ? 3 : 36;
        case ObjectKind::Mouth:
            switch (object.dir) {
                case 1: return 22;
                case 2: return 24;
                case 3: return 16;
                case 4: return 11;
                case 5: return 115;
                case 6: return 45;
                case 7: {
                    const uint16_t frames[] = {81, 91, 80, 82};
                    return frames[(uint8_t)clamp((float)mouthFrame_, 0.0f, 3.0f)];
                }
                case 31: return 51;
                default: return 23;
            }
        case ObjectKind::TopLid:
        case ObjectKind::BottomLid:
            return 34;
        case ObjectKind::At:
            return 40;
        case ObjectKind::V:
            return object.dir == 16 ? 19 : 2;
        case ObjectKind::Blush:
            return 66;
        case ObjectKind::Sb:
            return 53;
        case ObjectKind::Poke: {
            const uint16_t dir0[] = {97, 98, 99, 100, 101, 102, 103, 104};
            const uint16_t dir7[] = {68, 65, 67, 21, 52, 71, 54, 49};
            const uint16_t dir9[] = {27, 47, 50, 55, 63, 69, 70, 73};
            const uint16_t dir16[] = {72, 56, 57, 58, 59, 60, 61, 62};
            int frame = (int)clamp((float)pokeFrame_, 0.0f, 7.0f);
            if (object.dir == 7) return dir7[frame];
            if (object.dir == 9) return dir9[frame];
            if (object.dir == 16) return dir16[frame];
            return dir0[frame];
        }
        case ObjectKind::MenuButton: {
            const uint16_t frames[] = {93, 106, 107, 79};
            return frames[(uint8_t)clamp((float)menuFrame_, 0.0f, 3.0f)];
        }
    }
    return 0;
}

float OuORuntime::screenToDesignX(float screenX) const {
    return (screenX - OriginX) / DesignScale;
}

float OuORuntime::screenToDesignY(float screenY) const {
    return (screenY - OriginY) / DesignScale;
}

float OuORuntime::designToScreenX(float designX) const {
    return OriginX + designX * DesignScale;
}

float OuORuntime::designToScreenY(float designY) const {
    return OriginY + designY * DesignScale;
}

float OuORuntime::idleTime() const {
    return config_[AV_IDLE_TIME];
}

float OuORuntime::touchPressure() const {
    return config_[AV_TOUCH_COUNT];
}

int OuORuntime::mouthDirection() const {
    return mouth_.dir;
}

bool OuORuntime::isMenuButtonTap(float screenX, float screenY) const {
    float designX = screenToDesignX(screenX);
    float designY = screenToDesignY(screenY);
    return !menuHidden_ && designX >= 1168.0f && designX <= DesignW && designY >= 650.0f && designY <= DesignH;
}

bool OuORuntime::inZone(float x1, float y1, float x2, float y2) const {
    return touchDesignX_ >= x1 && touchDesignX_ <= x2 && touchDesignY_ >= y1 && touchDesignY_ <= y2;
}

bool OuORuntime::isEyeZone(float x, float y) const {
    return (fabsf(x - 252.0f) <= 130.0f || fabsf(x - 1029.0f) <= 130.0f) && fabsf(y - 301.0f) <= 140.0f;
}

bool OuORuntime::touchOverObject(const CtfObject& object, int imageHandle) const {
    float hotX = imageHandle == 34 ? 102.0f : 100.0f;
    float hotY = imageHandle == 34 ? 51.0f : 100.0f;
    float width = hotX * 2.0f;
    float height = hotY * 2.0f;
    float left = object.x - hotX * object.scaleX;
    float top = object.y - hotY * object.scaleY;
    return touchDesignX_ >= left && touchDesignX_ <= left + width * object.scaleX
        && touchDesignY_ >= top && touchDesignY_ <= top + height * object.scaleY;
}

bool OuORuntime::shouldRunOriginalGlobalAngleGate() const {
    int divisor = (int)lroundf(globals_[8] + 1.0f);
    if (divisor <= 0) {
        divisor = 1;
    }
    return ((int)lroundf(globals_[7]) % divisor) == 0;
}

float OuORuntime::sideFromFrameCenter(float x) const {
    float delta = x - DesignW * 0.5f;
    if (fabsf(delta) < 0.001f) {
        return 1.0f;
    }
    return delta / fabsf(delta);
}

void OuORuntime::setDirection(CtfObject& object, int mask) {
    int direction = directionFromMask(mask);
    int oldDirection = object.dir;
    object.dir = direction;
    if (object.kind == ObjectKind::Mouth && oldDirection != direction && direction != 7) {
        mouthFrame_ = 0;
    }
}

void OuORuntime::setDirection(CtfObject& first, CtfObject& second, int mask) {
    int direction = directionFromMask(mask);
    first.dir = direction;
    second.dir = direction;
}

void OuORuntime::setDirectionDirect(CtfObject& object, int direction) {
    object.dir = (int)clamp((float)direction, 0.0f, 31.0f);
}

int OuORuntime::directionFromMask(int mask) const {
    if (mask == INT32_MIN) {
        return 31;
    }
    if (mask == 0) {
        return 0;
    }
    for (int i = 0; i < 31; ++i) {
        if ((mask & (1 << i)) != 0) {
            return i;
        }
    }
    return 0;
}

void OuORuntime::setScale(CtfObject& object, float value) {
    object.scaleX = value;
    object.scaleY = value;
}

void OuORuntime::setScale(CtfObject& first, CtfObject& second, float value) {
    setScale(first, value);
    setScale(second, value);
}

void OuORuntime::setScaleY(CtfObject& first, CtfObject& second, float value) {
    first.scaleY = value;
    second.scaleY = value;
}

void OuORuntime::setAlterable(CtfObject& first, CtfObject& second, int index, float value) {
    first.av[index] = value;
    second.av[index] = value;
}

float OuORuntime::clamp(float value, float minValue, float maxValue) const {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

int OuORuntime::randomInt(int bound) {
    if (bound <= 0) {
        return 0;
    }
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (int)(rng_ % (uint32_t)bound);
}

const char* moodName(Mood mood) {
    switch (mood) {
        case Mood::Smile: return "smile";
        case Mood::Angry: return "grump";
        case Mood::Surprise: return "surprise";
        case Mood::Squint: return "blink";
        case Mood::Sad: return "sad";
        case Mood::Blank: return "blank";
        case Mood::Upset: return "upset";
        case Mood::Blink: return "blink2";
        case Mood::Cheeky: return "cheeky";
        case Mood::Frown: return "frown";
    }
    return "smile";
}

bool parseMoodName(const char* text, Mood& mood) {
    if (text == nullptr) {
        return false;
    }
    struct Entry {
        const char* name;
        Mood mood;
    };
    const Entry entries[] = {
        {"smile", Mood::Smile}, {":d", Mood::Smile},
        {"grump", Mood::Angry}, {"angry", Mood::Angry},
        {"surprise", Mood::Surprise}, {"*a*", Mood::Surprise},
        {"blink", Mood::Squint}, {"squint", Mood::Squint},
        {"sad", Mood::Sad},
        {"blank", Mood::Blank}, {":(", Mood::Blank},
        {"upset", Mood::Upset}, {":<", Mood::Upset},
        {"blink2", Mood::Blink},
        {"cheeky", Mood::Cheeky},
        {"frown", Mood::Frown},
    };
    for (const Entry& entry : entries) {
        if (strcasecmp(text, entry.name) == 0) {
            mood = entry.mood;
            return true;
        }
    }
    return false;
}
