// Frogger: SFML (graphics) + SFPhysics (collision detection)
//   SFML 2.x   : https://www.sfml-dev.org
//   SFPhysics  : https://github.com/profK/SFPhysics   (NuGet package "SFPhysics")
// Works with C++14 or newer. Keep the 'images' folder next to the program / project.
//
// How physics is used: cars, logs, turtles and the five lily pads are static PhysicsRectangles (the game
// slides the moving ones along by hand); the frog is a PhysicsRectangle too. World::UpdatePhysics()
// detects the overlaps and fires the frog's onCollision hook, which tells the game "hit a car",
// "standing on something floating (moving at speed X)" or "touching lily pad #n".
//
// Rules: 3 lives, 60 seconds per life, you must land on an empty lily pad in the top row, fill all five
// to advance a level. Type the word KONAMI = one extra life per game.

#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>
#include <SFPhysics.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define GETCWD _getcwd
#else
#include <unistd.h>
#define GETCWD getcwd
#endif

using namespace sf;
using namespace sfp;

// ======================= YOUR IMAGES =======================
// Put your pictures in the "images" folder (PNG, JPG, BMP, GIF, TGA). Any size works: they are scaled
// to fit, and empty transparent borders are trimmed automatically. Transparent PNGs look best.
// A name can list alternatives separated by | (the first one that exists is used).
const char* FROG_IMAGE = "images/frog.png";        // should face UP
const char* TURTLE_IMAGE = "images/turtle.png";      // rideable, drawn in groups of 3
const char* LOG_LONG_IMAGE = "images/log_long.png|images/lLog.png";
const char* LOG_SHORT_IMAGE = "images/log_short.png|images/sLog.png";
const char* CAR1_IMAGE = "images/car1.png";
const char* CAR2_IMAGE = "images/car2.png";
const char* RIVER_IMAGE = "images/river.png";       // stretched over the whole river
const char* ROAD_IMAGE = "images/road.png";        // tiled
const char* GRASS_IMAGE = "images/grass.png";       // tiled
const char* LILYPAD_IMAGE = "images/lilypad.png";     // the five goal pads

// Which way each picture points. If something drives backwards, flip its flag.
const bool CAR1_FACES_RIGHT = false;
const bool CAR2_FACES_RIGHT = true;
const bool TURTLE_FACES_RIGHT = false;

// ======================= YOUR SOUNDS =======================
// Put them in the "sfx" folder. SFML plays WAV, OGG and FLAC (NOT mp3 - convert mp3s to wav first).
// A missing file is fine: the game just runs without that sound.
const char* SFX_HOP = "sfx/frogger_hop.wav";        // every hop
const char* SFX_SQUASH = "sfx/frogger_squashed.wav";   // hit by a car (or out of time)
const char* SFX_DROWN = "sfx/drown.wav";              // fell in the river / missed a lily pad
const char* SFX_EXTRALIFE = "sfx/extra_life.wav";         // typed KONAMI
const char* SFX_FASTCAR = "sfx/fastcar.wav";            // a speeding car races in
const char* SFX_TIMELOW = "sfx/timelow.wav";            // time is running out
// ===========================================================

const int T = 48, COLS = 13, ROWS = 9, HUD = 64, W = COLS * T, H = ROWS * T + HUD;
// Rows: 0 lily pads | 1-3 river | 4 grass | 5-7 road | 8 start | (HUD strip underneath)

const int NUM_PADS = 5;
const int PAD_COLS[NUM_PADS] = { 1, 4, 6, 8, 11 };   // which columns have a lily pad
const float TIME_LIMIT = 60.f;                      // seconds per life
const int START_LIVES = 3;
const float LOW_TIME = 10.f;             // the warning starts when this many seconds are left...
const float LOW_TIME_REPEAT = 3.f;       // ...and repeats this often
const float FAST_CAR_SPEED = 130.f;      // lanes at least this fast play the "fast car" sound
const float FAST_CAR_COOLDOWN = 4.f;     // seconds between those sounds

enum Kind { CAR, FLOAT, PAD };             // FLOAT = log or turtle: safe to stand on
struct Info { Kind kind; float speed; int id = 0; };   // what a body is; looked up by the body's address
enum class State { Title, Playing, GameOver };
enum class Death { Car, Water, Time };   // decides which death sound plays

// Type the word K-O-N-A-M-I for a bonus life
const Keyboard::Key KONAMI[6] = { Keyboard::K, Keyboard::O, Keyboard::N, Keyboard::A, Keyboard::M, Keyboard::I };

struct Obj {                               // one car / log / turtle group
    PhysicsRectangle body;
    std::vector<Sprite> parts;             // a turtle group has several sprites
    std::vector<float> offsets;            // each part's x offset from the object's centre
    float x = 0, y = 0, speed = 0, len = 0;
    bool fast = false;                     // a speeding car (plays the fast-car sound)
    int w = 0;
};

// SFPhysics 1.x shapes contain a body (shape.getBody()); in 2.x the shape IS the body.
// This helper compiles against either version.
template <class S> auto bodyOf(S& s, int) -> decltype(s.getBody()) { return s.getBody(); }
template <class S> PhysicsBody& bodyOf(S& s, long) { return s; }
template <class S> auto& bodyOf(S& s) { return bodyOf(s, 0); }

bool fileExists(const std::string& p) { std::ifstream f(p, std::ios::binary); return f.good(); }

// Looks for the file here, then in up to four parent folders. That way it still works when Visual
// Studio starts the program from x64/Debug (images two folders up) or from the project/solution folder.
std::string findFile(const std::string& path) {
    static const char* prefixes[] = { "", "../", "../../", "../../../", "../../../../" };
    for (const char* p : prefixes) {
        std::string full = std::string(p) + path;
        if (fileExists(full)) return full;
    }
    return path;
}

[[noreturn]] void fail(const std::string& what) {
    char cwd[1024] = "?";
    GETCWD(cwd, sizeof cwd);
    std::cerr << "\nCan't load image: " << what << "\n"
        << "Looked in the working directory:  " << cwd << "\n"
        << "(and up to 4 folders above it). Put the 'images' folder in one of those places,\n"
        << "or check that the file exists and is a valid image.\n\n"
        << "Press Enter to close...";
    std::cin.get();
    std::exit(1);
}

// 'names' may list alternatives separated by '|'. Transparent borders are trimmed off, so pictures
// with big empty margins still draw at the right size; 'square' pads the result to a square.
Texture loadTex(const std::string& names, bool square = false, bool trim = true) {
    std::string chosen, first;
    size_t start = 0;
    while (true) {
        size_t bar = names.find('|', start);
        std::string one = names.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (first.empty()) first = one;
        std::string full = findFile(one);
        if (fileExists(full)) { chosen = full; break; }
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    Image img;
    if (chosen.empty() || !img.loadFromFile(chosen)) fail(first);

    Vector2u sz = img.getSize();
    unsigned x0 = sz.x, y0 = sz.y, x1 = 0, y1 = 0;
    bool any = false;
    if (trim) {
        for (unsigned y = 0; y < sz.y; y++)
            for (unsigned x = 0; x < sz.x; x++)
                if (img.getPixel(x, y).a > 10) {
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    y0 = std::min(y0, y); y1 = std::max(y1, y);
                    any = true;
                }
    }
    if (!any) { x0 = 0; y0 = 0; x1 = sz.x - 1; y1 = sz.y - 1; }
    IntRect r((int)x0, (int)y0, (int)(x1 - x0 + 1), (int)(y1 - y0 + 1));

    Texture t;
    bool ok;
    if (square && r.width != r.height) {
        int side = std::max(r.width, r.height);
        Image sq;
        sq.create(side, side, Color::Transparent);
        sq.copy(img, (side - r.width) / 2, (side - r.height) / 2, r, false);
        ok = t.loadFromImage(sq);
    }
    else {
        ok = t.loadFromImage(img, r);
    }
    if (!ok) fail(first);
    t.setSmooth(true);
    return t;
}

// One sound effect. Optional: if the file is missing or can't be decoded the game just stays quiet.
struct Sfx {
    SoundBuffer buffer;      // declared first so it outlives the Sound that plays it
    Sound sound;
    bool loaded = false;
    Sfx() = default;
    Sfx(const Sfx&) = delete;
    Sfx& operator=(const Sfx&) = delete;
    void load(const char* file, float volume = 100.f) {   // volume 0-100
        std::string full = findFile(file);
        if (!fileExists(full) || !buffer.loadFromFile(full)) {
            std::cerr << "Sound not loaded (the game will run without it): " << file << "\n";
            return;
        }
        sound.setBuffer(buffer);
        sound.setVolume(volume);
        loaded = true;
    }
    void play() { if (loaded) sound.play(); }   // play() restarts the sound if it's already playing
    void stop() { if (loaded) sound.stop(); }
};

// Scale + centre a sprite on its texture's middle. Stretch to w x h, or (keepAspect) fit inside it.
// Returns the size it ends up drawn at.
Vector2f fit(Sprite& s, const Texture& t, float w, float h, bool keepAspect, bool flip) {
    Vector2u sz = t.getSize();
    float sx = w / sz.x, sy = h / sz.y;
    if (keepAspect) sx = sy = std::min(sx, sy);
    s.setTexture(t);
    s.setOrigin(sz.x / 2.f, sz.y / 2.f);
    s.setScale(flip ? -sx : sx, sy);
    return Vector2f(sz.x * sx, sz.y * sy);
}

int main() {
    RenderWindow window(VideoMode(W, H), "Frogger");
    window.setFramerateLimit(60);
    window.setKeyRepeatEnabled(false);

    Texture frogT = loadTex(FROG_IMAGE, true), turtleT = loadTex(TURTLE_IMAGE, true),
        logLongT = loadTex(LOG_LONG_IMAGE), logShortT = loadTex(LOG_SHORT_IMAGE),
        car1T = loadTex(CAR1_IMAGE), car2T = loadTex(CAR2_IMAGE),
        riverT = loadTex(RIVER_IMAGE, false, false), roadT = loadTex(ROAD_IMAGE, false, false),
        grassT = loadTex(GRASS_IMAGE, false, false), padT = loadTex(LILYPAD_IMAGE);

    // ---- physics world (no gravity: top-down game)
    World world(Vector2f(0, 0));

    // Remembers what each physics body is (car / log / turtle group), keyed by the body's address.
    // (Deque elements never move, so the addresses stay valid.)
    std::unordered_map<const PhysicsBody*, Info> infoOf;

    // ---- lanes: every object is a static physics rectangle that we slide by hand
    // tiles > 0 means "a group of that many square sprites" (turtles); otherwise one sprite of width w.
    struct LaneDef { int row; float speed; int w; int gap; Kind kind; Texture* tex; bool facesRight; int tiles; };
    LaneDef defs[] = {
        {1, +70,  144,   160, FLOAT, &logShortT, true,               0},   // short logs  ->
        {2, -100, 3 * T, 170, FLOAT, &turtleT,   TURTLE_FACES_RIGHT, 3},   // 3 turtles    <-
        {3, +55,  240,   150, FLOAT, &logLongT,  true,               0},   // long logs    ->
        {5, -120, 72,    220, CAR,   &car1T,     CAR1_FACES_RIGHT,   0},
        {6, +90,  72,    250, CAR,   &car2T,     CAR2_FACES_RIGHT,   0},
        {7, +140, 72,    200, CAR,   &car1T,     CAR1_FACES_RIGHT,   0},
    };
    std::deque<Obj> objs;  // deque: references stay valid while we add objects
    for (const LaneDef& d : defs) {
        float period = (float)(d.w + d.gap);
        int count = (int)std::ceil((W + d.w) / period);
        for (int i = 0; i < count; i++) {
            objs.emplace_back();
            Obj& o = objs.back();
            o.w = d.w; o.speed = d.speed; o.len = count * period;
            o.fast = (d.kind == CAR && std::fabs(d.speed) >= FAST_CAR_SPEED);
            o.x = -d.w / 2.f + i * period;
            o.y = d.row * T + T / 2.f;

            bool flip = (d.speed > 0) != d.facesRight;   // flip when the picture points the wrong way
            Vector2f drawn;
            if (d.tiles > 0) {
                for (int k = 0; k < d.tiles; k++) {
                    o.parts.emplace_back();
                    drawn = fit(o.parts.back(), *d.tex, (float)T, (float)T, true, flip);
                    o.offsets.push_back((k - (d.tiles - 1) / 2.f) * T);
                }
                drawn.x = (float)d.w;
            }
            else {
                o.parts.emplace_back();
                drawn = fit(o.parts.back(), *d.tex, (float)d.w, (float)T, true, flip);
                o.offsets.push_back(0.f);
            }
            // The physics box matches what is drawn (a little smaller so hits feel fair).
            o.body.setSize(Vector2f(std::max(16.f, drawn.x - 8.f), std::min(32.f, drawn.y)));
            o.body.setCenter(Vector2f(o.x, o.y));
            bodyOf(o.body).setStatic(true);
            infoOf[&bodyOf(o.body)] = Info{ d.kind, d.speed };
            world.AddPhysicsBody(bodyOf(o.body));
        }
    }

    // ---- lily pads (the goal): static physics rectangles on row 0
    std::deque<PhysicsRectangle> pads;
    for (int i = 0; i < NUM_PADS; i++) {
        pads.emplace_back();
        PhysicsRectangle& p = pads.back();
        p.setSize(Vector2f(36.f, 36.f));
        p.setCenter(Vector2f(PAD_COLS[i] * T + T / 2.f, T / 2.f));
        bodyOf(p).setStatic(true);
        infoOf[&bodyOf(p)] = Info{ PAD, 0.f, i };
        world.AddPhysicsBody(bodyOf(p));
    }

    // ---- the frog
    PhysicsRectangle frogBody;
    frogBody.setSize(Vector2f(24, 24));
    world.AddPhysicsBody(bodyOf(frogBody));
    const PhysicsBody* frogPtr = &bodyOf(frogBody);
    Sprite frogSprite;
    fit(frogSprite, frogT, (float)T, (float)T, true, false);
    const Vector2f frogScale = frogSprite.getScale();

    // ---- river: one picture stretched over rows 1-3
    Sprite riverSprite(riverT);
    riverSprite.setScale((float)W / riverT.getSize().x, 3.f * T / riverT.getSize().y);
    riverSprite.setPosition(0.f, (float)T);

    // ---- a small square of river behind each lily pad
    int slotW = std::max(1, (int)(riverT.getSize().x / COLS)), slotH = std::max(1, (int)(riverT.getSize().y / 3));
    Sprite waterSlot(riverT);
    waterSlot.setTextureRect(IntRect(0, 0, slotW, slotH));
    waterSlot.setScale((float)T / slotW, (float)T / slotH);

    Sprite padSprite, padFrog, lifeIcon;
    fit(padSprite, padT, (float)T, (float)T, true, false);
    fit(padFrog, frogT, (float)T, (float)T, true, false);
    fit(lifeIcon, frogT, 32.f, 32.f, true, false);

    // ---- text (optional): uses the first font it can find; without one the game still plays
    Font font;
    bool haveFont = false;
    {
        static const char* paths[] = { "images/font.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
                                      "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf" };
        for (const char* p : paths)
            if (fileExists(p) && font.loadFromFile(p)) { haveFont = true; break; }
    }
    auto drawText = [&](const std::string& str, float x, float y, unsigned size, Color col, bool center) {
        if (!haveFont) return;
        Text t(str, font, size);
        t.setFillColor(col);
        t.setOutlineColor(Color::Black);
        t.setOutlineThickness(2.f);
        FloatRect b = t.getLocalBounds();
        t.setPosition(center ? (W - b.width) / 2.f - b.left : x, y);
        window.draw(t);
        };

    // ---- sound effects
    Sfx hopSfx, squashSfx, drownSfx, extraLifeSfx, fastCarSfx, timeLowSfx;
    hopSfx.load(SFX_HOP);
    squashSfx.load(SFX_SQUASH, 80.f);
    drownSfx.load(SFX_DROWN, 60.f);
    extraLifeSfx.load(SFX_EXTRALIFE);
    fastCarSfx.load(SFX_FASTCAR);
    timeLowSfx.load(SFX_TIMELOW);

    // ---- game state
    State state = State::Title;
    float fx = 6.5f * T, angle = 0, timeLeft = TIME_LIMIT, deadT = 0, bonusT = 0, lowT = 0, fastCool = 0;
    int row = ROWS - 1, maxRow = ROWS - 1;
    int score = 0, lives = START_LIVES, level = 1;
    bool filled[NUM_PADS] = {};
    bool konamiUsed = false;
    std::vector<Keyboard::Key> typed;
    bool hitCar = false, onFloat = false;
    float floatSpeed = 0;
    int padHit = -1;

    // Collision hook: called by World::UpdatePhysics when the frog overlaps something.
    bodyOf(frogBody).onCollision = [&](const auto& r) {
        const PhysicsBody* other = (&r.object1 == frogPtr) ? &r.object2 : &r.object1;   // the one that isn't the frog
        auto it = infoOf.find(other);
        if (it == infoOf.end()) return;
        switch (it->second.kind) {
        case CAR:   hitCar = true; break;
        case FLOAT: onFloat = true; floatSpeed = it->second.speed; break;
        case PAD:   padHit = it->second.id; break;
        }
        };

    auto resetFrog = [&] { fx = 6.5f * T; row = ROWS - 1; maxRow = ROWS - 1; angle = 0; timeLeft = TIME_LIMIT; lowT = 0; timeLowSfx.stop(); };
    auto resetGame = [&] {
        score = 0; lives = START_LIVES; level = 1; deadT = 0; konamiUsed = false;
        for (bool& f : filled) f = false;
        resetFrog();
        };
    auto die = [&](Death why) {                // lose a life (the frog flashes red for a moment first)
        if (deadT > 0) return;
        lives--;
        deadT = 0.8f;
        timeLowSfx.stop();
        (why == Death::Water ? drownSfx : squashSfx).play();
        };

    Clock clock;
    std::string lastTitle;
    while (window.isOpen()) {
        // ---- input
        Event e;
        while (window.pollEvent(e)) {
            if (e.type == Event::Closed) window.close();
            if (e.type != Event::KeyPressed) continue;
            Keyboard::Key k = e.key.code;

            // Type "konami" for one bonus life per game (works on the title screen or mid-game)
            bool konamiDone = false;
            typed.push_back(k);
            if (typed.size() > 6) typed.erase(typed.begin());
            if (typed.size() == 6 && std::equal(typed.begin(), typed.end(), KONAMI)) {
                typed.clear();
                konamiDone = true;
                if (!konamiUsed && state != State::GameOver) { konamiUsed = true; lives++; bonusT = 2.5f; extraLifeSfx.play(); }
            }

            if (k == Keyboard::Escape) window.close();
            if (k == Keyboard::Space || k == Keyboard::Return) {
                if (state == State::Title) { state = State::Playing; resetFrog(); }
                else if (state == State::GameOver) { resetGame(); state = State::Playing; }
            }
            if (state != State::Playing || deadT > 0 || konamiDone) continue;

            int oldRow = row;
            float oldFx = fx;
            switch (k) {
            case Keyboard::Up:
                if (row > 0) { row--; angle = 0; if (row < maxRow) { maxRow = row; score += 10; } }
                break;
            case Keyboard::Down: if (row < ROWS - 1) row++; angle = 180; break;
            case Keyboard::Left: fx -= T; angle = -90; break;
            case Keyboard::Right: fx += T; angle = 90; break;
            default: break;
            }
            fx = std::max(T / 2.f, std::min(W - T / 2.f, fx));
            if (row != oldRow || fx != oldFx) hopSfx.play();   // only if the frog actually moved
        }

        float dt = std::min(0.05f, clock.restart().asSeconds());
        float mul = std::min(2.5f, 1.f + 0.15f * (level - 1));   // traffic speeds up each level
        if (bonusT > 0) bonusT -= dt;

        // ---- timers
        if (state == State::Playing) {
            if (deadT > 0) {
                deadT -= dt;
                if (deadT <= 0) {
                    if (lives <= 0) state = State::GameOver;
                    else resetFrog();
                }
            }
            else {
                timeLeft -= dt;
                if (timeLeft <= 0) die(Death::Time);        // out of time: lose a life
                else if (timeLeft <= LOW_TIME) {            // time is running out: warning beeps
                    lowT -= dt;
                    if (lowT <= 0) { timeLowSfx.play(); lowT = LOW_TIME_REPEAT; }
                }
            }
        }

        // ---- move lane objects, then let the physics world detect collisions
        if (fastCool > 0) fastCool -= dt;
        for (Obj& o : objs) {
            o.x += o.speed * mul * dt;
            bool wrapped = false;                 // true when it loops round and re-enters the screen
            if (o.speed > 0 && o.x >= o.len - o.w / 2.f) { o.x -= o.len; wrapped = true; }
            if (o.speed < 0 && o.x < -o.w / 2.f) { o.x += o.len; wrapped = true; }
            if (wrapped && o.fast && state == State::Playing && fastCool <= 0) {
                fastCarSfx.play();
                fastCool = FAST_CAR_COOLDOWN;
            }
            o.body.setCenter(Vector2f(o.x, o.y));
            for (size_t i = 0; i < o.parts.size(); i++) o.parts[i].setPosition(o.x + o.offsets[i], o.y);
        }
        float fy = row * T + T / 2.f;
        frogBody.setCenter(Vector2f(fx, fy));   // the game owns the frog's position
        hitCar = onFloat = false;
        padHit = -1;
        world.UpdatePhysics((unsigned long)std::max(1.f, dt * 1000.f));

        // ---- react to what the physics world reported
        if (state == State::Playing && deadT <= 0) {
            bool dead = false;
            Death why = Death::Water;                       // river and missed pads are "water"
            if (hitCar) { dead = true; why = Death::Car; }
            if (row >= 1 && row <= 3) {
                if (onFloat) fx += floatSpeed * mul * dt;   // ride the log / turtles
                else dead = true;                           // water!
                if (fx < 0 || fx > W) dead = true;
            }
            if (row == 0) {                                 // must land squarely on an EMPTY lily pad
                bool landed = padHit >= 0 && !filled[padHit] &&
                    std::fabs(fx - (PAD_COLS[padHit] * T + T / 2.f)) <= 22.f;
                if (landed) {
                    filled[padHit] = true;
                    score += 50 + (int)timeLeft * 2;        // bonus for speed
                    bool all = true;
                    for (bool f : filled) all = all && f;
                    if (all) {
                        level++;
                        score += 200;
                        for (bool& f : filled) f = false;
                    }
                    resetFrog();
                }
                else {
                    dead = true;                            // missed the pad, or it was already taken
                }
            }
            if (dead) die(why);
        }

        // ---- window title
        std::string title = "Frogger  -  Score: " + std::to_string(score) + "   Lives: " + std::to_string(lives) +
            "   Level: " + std::to_string(level) + "   Time: " + std::to_string((int)std::ceil(std::max(0.f, timeLeft)));
        if (state == State::Title) title = "Frogger  -  press SPACE to start";
        if (state == State::GameOver) title = "Frogger  -  GAME OVER (score " + std::to_string(score) + ")  -  press SPACE";
        if (bonusT > 0) title += "   +1 LIFE!";
        if (title != lastTitle) { window.setTitle(title); lastTitle = title; }

        // ---- draw (SFML)
        window.clear();
        Sprite tile;
        for (int r = 0; r < ROWS; r++) {
            if (r >= 1 && r <= 3) continue;       // the river picture covers these rows
            tile.setTexture((r >= 5 && r <= 7) ? roadT : grassT, true);
            tile.setScale((float)T / tile.getTexture()->getSize().x, (float)T / tile.getTexture()->getSize().y);
            for (int c = 0; c < COLS; c++) { tile.setPosition((float)c * T, (float)r * T); window.draw(tile); }
        }
        for (int i = 0; i < NUM_PADS; i++) {          // water slot + lily pad (+ a frog once it's taken)
            waterSlot.setPosition((float)PAD_COLS[i] * T, 0.f);
            window.draw(waterSlot);
            padSprite.setPosition(PAD_COLS[i] * T + T / 2.f, T / 2.f);
            window.draw(padSprite);
            if (filled[i]) { padFrog.setPosition(PAD_COLS[i] * T + T / 2.f, T / 2.f); window.draw(padFrog); }
        }
        window.draw(riverSprite);
        for (const Obj& o : objs)
            for (const Sprite& s : o.parts) window.draw(s);
        if (state == State::Playing) {
            frogSprite.setScale(frogScale);
            frogSprite.setPosition(fx, row * T + T / 2.f);
            frogSprite.setRotation(angle);
            frogSprite.setColor(deadT > 0 ? Color(255, 60, 60) : Color::White);   // red while dying
            window.draw(frogSprite);
        }

        // HUD strip under the playfield. Top row: lives + score + level. Bottom row: the time bar,
        // full width on its own line, so it can never overlap the text.
        RectangleShape hudBg(Vector2f((float)W, (float)HUD));
        hudBg.setPosition(0.f, (float)ROWS * T);
        hudBg.setFillColor(Color(15, 15, 20));
        window.draw(hudBg);
        for (int i = 0; i < lives; i++) {
            lifeIcon.setPosition(8.f + i * 36 + 16.f, ROWS * T + 22.f);
            window.draw(lifeIcon);
        }
        drawText("SCORE " + std::to_string(score) + "     LEVEL " + std::to_string(level), 200.f, ROWS * T + 8.f, 22, Color::White, false);
        float frac = std::max(0.f, timeLeft / TIME_LIMIT);
        const float barW = (float)W - 16.f;
        RectangleShape barBack(Vector2f(barW, 12.f)), bar(Vector2f(barW * frac, 12.f));
        barBack.setPosition(8.f, ROWS * T + 46.f);
        bar.setPosition(8.f, ROWS * T + 46.f);
        barBack.setFillColor(Color(60, 60, 60));
        bar.setFillColor(frac > 0.33f ? Color(110, 230, 90) : frac > 0.16f ? Color(250, 210, 60) : Color(230, 60, 50));
        window.draw(barBack);
        window.draw(bar);

        if (bonusT > 0) drawText("+1 LIFE!", 0, ROWS * T / 2.f - 20.f, 40, Color(250, 210, 60), true);

        if (state != State::Playing) {                // title / game-over overlay
            RectangleShape shade(Vector2f((float)W, (float)ROWS * T));
            shade.setFillColor(Color(0, 0, 0, 170));
            window.draw(shade);
            if (state == State::Title) {
                drawText("FROGGER", 0, 90.f, 64, Color(110, 230, 90), true);
                drawText("Arrow keys to hop", 0, 190.f, 24, Color::White, true);
                drawText("Land on an empty lily pad. 3 lives, 60 seconds each.", 0, 225.f, 20, Color::White, true);
                drawText("Press SPACE to start", 0, 290.f, 28, Color(250, 210, 60), true);
            }
            else {
                drawText("GAME OVER", 0, 110.f, 64, Color(230, 60, 50), true);
                drawText("Score: " + std::to_string(score), 0, 210.f, 32, Color::White, true);
                drawText("Press SPACE to play again", 0, 280.f, 26, Color(250, 210, 60), true);
            }
            if (bonusT > 0) drawText("+1 LIFE!", 0, 340.f, 36, Color(250, 210, 60), true);
        }
        window.display();
    }
    return 0;
}