#pragma once

#include <stdint.h>

enum class Mood : uint8_t {
    Smile = 0,
    Angry,
    Surprise,
    Squint,
    Sad,
    Blank,
    Upset,
    Blink,
    Cheeky,
    Frown,
};

enum class ObjectKind : uint8_t {
    Eye,
    Mouth,
    TopLid,
    BottomLid,
    At,
    V,
    Blush,
    Sb,
    Poke,
    MenuButton,
};

struct CtfObject {
    ObjectKind kind;
    float startX;
    float startY;
    float av[16];
    bool flags[16];
    float x;
    float y;
    float scaleX;
    float scaleY;
    float angle;
    int dir;
    bool visible;

    CtfObject(ObjectKind objectKind, float initialX, float initialY)
        : kind(objectKind),
          startX(initialX),
          startY(initialY),
          av{},
          flags{},
          x(initialX),
          y(initialY),
          scaleX(1.0f),
          scaleY(1.0f),
          angle(0.0f),
          dir(0),
          visible(true) {}

    void reset();
};

struct DrawCommand {
    ObjectKind kind;
    uint16_t imageHandle;
    float x;
    float y;
    float scaleX;
    float scaleY;
    float angle;
    uint8_t alpha;
};

struct DrawList {
    static constexpr uint8_t MaxCommands = 24;
    DrawCommand commands[MaxCommands];
    uint8_t count = 0;

    void clear() { count = 0; }
    void add(const DrawCommand& command) {
        if (count < MaxCommands) {
            commands[count++] = command;
        }
    }
};

class OuORuntime {
public:
    static constexpr float DesignW = 1280.0f;
    static constexpr float DesignH = 720.0f;
    static constexpr float ScreenW = 240.0f;
    static constexpr float ScreenH = 240.0f;
    static constexpr float DesignScale = ScreenW / DesignW;
    static constexpr float OriginX = 0.0f;
    static constexpr float OriginY = (ScreenH - DesignH * DesignScale) * 0.5f;
    static constexpr float TickMs = 1000.0f / 60.0f;

    OuORuntime();

    void reset();
    void tick();
    void setAcceleration(float x, float y, float z, uint32_t nowMs);
    void touchDown(float screenX, float screenY);
    void touchMove(float screenX, float screenY);
    void touchUp(float screenX, float screenY);
    void touchCancel();

    void setMood(Mood mood);
    Mood mood() const { return mood_; }

    void setMenuHidden(bool hidden);
    bool menuHidden() const { return menuHidden_; }

    void setBlush(bool enabled);
    bool blushEnabled() const { return persistentBlush_; }

    void setMouthStretchingEnabled(bool enabled);
    bool mouthStretchingEnabled() const { return mouthStretchingEnabled_; }

    void setTiltWinkingEnabled(bool enabled) { tiltWinkingEnabled_ = enabled; }
    bool tiltWinkingEnabled() const { return tiltWinkingEnabled_; }

    void setLockEmotions(bool enabled);
    bool lockEmotions() const { return lockEmotions_; }

    void setStaticMood(bool enabled);
    bool staticMood() const { return staticMood_; }

    void setRandomSeed(uint32_t seed);
    void buildDrawList(DrawList& out) const;

    float screenToDesignX(float screenX) const;
    float screenToDesignY(float screenY) const;
    float designToScreenX(float designX) const;
    float designToScreenY(float designY) const;

    float idleTime() const;
    float touchPressure() const;
    int mouthDirection() const;
    bool isTouching() const { return touching_; }
    float lastAccelerationDelta() const { return lastAccelerationDelta_; }
    float shakeMeter() const { return shakeMeter_; }
    uint32_t shakeEvents() const { return shakeEvents_; }

private:
    static constexpr int AV_TOUCH_RESET = 0;
    static constexpr int AV_RESET_DELAY = 1;
    static constexpr int AV_IDLE_TIME = 2;
    static constexpr int AV_TOP_GRUMP = 3;
    static constexpr int AV_TOP_SAD = 4;
    static constexpr int AV_BOTTOM_CHEEKY = 5;
    static constexpr int AV_BOTTOM_FROWN = 6;
    static constexpr int AV_TOUCH_COUNT = 7;
    static constexpr int AV_SHAKE_COOLDOWN = 10;
    static constexpr int FLAG_POKE_ACTIVE = 0;
    static constexpr int FLAG_POKE_LEFT = 1;
    static constexpr int FLAG_POKE_RIGHT = 2;

    CtfObject eyeRight_;
    CtfObject eyeLeft_;
    CtfObject topLeft_;
    CtfObject bottomLeft_;
    CtfObject topRight_;
    CtfObject bottomRight_;
    CtfObject atLeft_;
    CtfObject atRight_;
    CtfObject vLeft_;
    CtfObject vRight_;
    CtfObject blushRight_;
    CtfObject blushLeft_;
    CtfObject sbLeft_;
    CtfObject sbRight_;
    CtfObject mouth_;
    CtfObject poke_;
    CtfObject menuButton_;

    float config_[16] = {};
    float globals_[9] = {};
    float lastAcceleration_[3] = {};

    Mood mood_ = Mood::Smile;
    bool menuHidden_ = false;
    bool persistentBlush_ = false;
    bool touching_ = false;
    float downX_ = 0.0f;
    float downY_ = 0.0f;
    float downDesignX_ = 0.0f;
    float downDesignY_ = 0.0f;
    float downIdleTime_ = 0.0f;
    float downTopSad_ = 0.0f;
    float touchDesignX_ = 0.0f;
    float touchDesignY_ = 0.0f;
    int menuFrame_ = 3;
    int pokeFrame_ = 0;
    int mouthFrame_ = 0;
    float autoBlinkMs_ = 0.0f;
    float autoMood600Ms_ = 0.0f;
    float autoMood1000Ms_ = 0.0f;
    float touchMovement250Ms_ = 0.0f;
    float idleSecondMs_ = 0.0f;
    float lockedIdleMs_ = 0.0f;
    float shakeMeter_ = 0.0f;
    float lastAccelerationDelta_ = 0.0f;
    bool hasAcceleration_ = false;
    bool tiltWinkArmed_ = false;
    uint32_t lastShakeMs_ = 0;
    uint32_t lastTiltWinkMs_ = 0;
    uint32_t shakeEvents_ = 0;
    bool mouthStretchingEnabled_ = true;
    bool tiltWinkingEnabled_ = true;
    bool lockEmotions_ = false;
    bool staticMood_ = false;
    bool sleeping_ = false;
    uint32_t rng_ = 0x12345678u;

    void executeMenuCommand(Mood command);
    void beginOriginalCommand(Mood command);
    void originalBlink(bool blink2);
    void runOriginalSmileEvent();
    void runOriginalGrumpEvent();
    void runOriginalUpsetEvent();
    void runOriginalSurpriseEvent();
    void runOriginalBlankEvent();
    void runOriginalSadEvent();
    void runOriginalCheekyEvent();
    void runOriginalFrownEvent();
    void handleOriginalClick(float screenX, float screenY);
    void maybeRunOriginalLongSadClickMode();
    void runOriginalTouchInteractions();
    void maybeStartOriginalMouthStretch();
    void startOriginalMouthStretch();
    void updateOriginalLidDrag();
    void applyOriginalTopLidDrag(CtfObject& lid);
    void applyOriginalBottomLidDrag(CtfObject& lid);
    void updateOriginalMouthStretch();
    void updateOriginalPokeHelper();
    void runOriginalMouthStretchCollapse();
    void runOriginalMouthDirectionCache();
    void runOriginalMouthNormalScaleReturn();
    void runOriginalMenuButtonEvents();
    void updatePokeAnimationFrame();
    void hidePokeHelper(bool resetSeeds);
    void maybeCollapsePokeHelper();
    void clearPokeFlags();
    void updateOriginalBlushPositions();
    void updateOriginalTouchLatch();
    void updateOriginalBlinkCounter();
    void updateOriginalBlinkCounter(CtfObject& eye, CtfObject& topLid, CtfObject& bottomLid);
    void updateOriginalAutoBlink();
    void runOriginalIdleRandomMoodEvents();
    void runOriginalFastLatchSmileEvent();
    void maybeRunOriginalLatchReleaseRandomEvent();
    void runOriginalLatchReleaseRandomEvent(int mouthMask);
    void applyOriginalEyeAngles();
    void applyOriginalEyeAngle(CtfObject& eye);
    void runOriginalVOverlayEvents();
    void showOriginalVOverlay(float scaleY);
    void maybeRunOriginalShakeEvent(uint32_t nowMs);
    void showOriginalAtOverlay();
    void maybeRunOriginalSideWink(float accelerometerX, uint32_t nowMs);
    void runOriginalSideWink(CtfObject& eye);
    void originalEyeClick();
    void selectMouthStretchFrame(int previousDirection);

    void addObject(DrawList& out, const CtfObject& object) const;
    uint16_t imageFor(const CtfObject& object) const;
    bool isMenuButtonTap(float screenX, float screenY) const;
    bool inZone(float x1, float y1, float x2, float y2) const;
    bool isEyeZone(float x, float y) const;
    bool touchOverObject(const CtfObject& object, int imageHandle) const;
    bool shouldRunOriginalGlobalAngleGate() const;
    float sideFromFrameCenter(float x) const;
    float originalStretchJitter();
    float seedValue(float value) const;
    int idleMoodChance(float pressureFactor) const;
    bool randomZero(int bound);
    float originalTouchMovementDistance(float dx, float dy) const;
    bool advanceTouchMovement250();
    void setDirection(CtfObject& object, int mask);
    void setDirection(CtfObject& first, CtfObject& second, int mask);
    void setDirectionDirect(CtfObject& object, int direction);
    int directionFromMask(int mask) const;
    void setScale(CtfObject& object, float value);
    void setScale(CtfObject& first, CtfObject& second, float value);
    void setScaleY(CtfObject& first, CtfObject& second, float value);
    void setAlterable(CtfObject& first, CtfObject& second, int index, float value);
    float clamp(float value, float minValue, float maxValue) const;
    int randomInt(int bound);
};

const char* moodName(Mood mood);
bool parseMoodName(const char* text, Mood& mood);
