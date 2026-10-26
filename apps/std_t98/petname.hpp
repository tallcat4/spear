#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <charconv>
namespace spear::std_t98 {
/*
 * Pet name dictionary v1
 *
 * 50 moods × 50 features × 50 animals
 * = 125,000 possible names.
 *
 * Do not reorder these tables after deployment.
 */

static const char *const moods[50] = {
    "Happy", "Lucky", "Sleepy", "Angry", "Funny",
    "Crazy", "Brave", "Shy", "Calm", "Kind",
    "Nice", "Gentle", "Friendly", "Lonely", "Quiet",
    "Noisy", "Busy", "Lazy", "Clever", "Smart",
    "Silly", "Proud", "Cool", "Sweet", "Lovely",
    "Cheerful", "Playful", "Hungry", "Tired", "Surprised",
    "Excited", "Scared", "Curious", "Careful", "Honest",
    "Polite", "Serious", "Wild", "Free", "Strong",
    "Weak", "Bold", "Bright", "Fresh", "Warm",
    "Poor", "Rich", "Super", "Magic", "Special"
};

static const char *const features[50] = {
    "Big", "Small", "Little", "Tiny", "Tall",
    "Short", "Long", "Round", "Flat", "Thin",
    "Thick", "Fat", "Wide", "Red", "Blue",
    "Green", "Yellow", "Black", "White", "Pink",
    "Brown", "Gray", "Gold", "Silver", "Orange",
    "Purple", "Dark", "Light", "Fast", "Slow",
    "Speedy", "Soft", "Hard", "Smooth", "Rough",
    "Clean", "Dirty", "Hot", "Cold", "Dry",
    "Wet", "Old", "Young", "New", "Giant",
    "Cute", "Fluffy", "Striped", "Spotted", "Shiny"
};

static const char *const animals[50] = {
    "Cat", "Dog", "Fox", "Wolf", "Bear",
    "Panda", "Tiger", "Lion", "Rabbit", "Mouse",
    "Hamster", "Squirrel", "Deer", "Horse", "Cow",
    "Pig", "Sheep", "Goat", "Monkey", "Gorilla",
    "Elephant", "Giraffe", "Zebra", "Hippo", "Rhino",
    "Camel", "Kangaroo", "Koala", "Penguin", "Dolphin",
    "Whale", "Shark", "Seal", "Otter", "Turtle",
    "Frog", "Snake", "Lizard", "Crocodile", "Eagle",
    "Owl", "Duck", "Chicken", "Swan", "Goose",
    "Parrot", "Sparrow", "Crow", "Bat", "Butterfly"
};

/*
 * MurmurHash3 32-bit finalizer.
 * No random seed or platform-dependent state.
 */
inline uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= UINT32_C(0x85ebca6b);
    x ^= x >> 13;
    x *= UINT32_C(0xc2b2ae35);
    x ^= x >> 16;
    return x;
}

/*
 * Convert unsigned int ID to a deterministic pet name.
 *
 * Returns the number of characters that would have
 * been written, excluding the terminating null byte.
 *
 * A return value >= size indicates truncation.
 */
inline int petname(uint32_t id, char *buf, size_t size)
{
    uint32_t h = mix32((uint32_t)id);

    // Map to one of 125,000 combinations.
    uint32_t n = h % UINT32_C(125000);

    unsigned int mood    = n / 2500;
    unsigned int feature = (n / 50) % 50;
    unsigned int animal  = n % 50;

    return snprintf(
        buf, size, "%s %s %s",
        moods[mood],
        features[feature],
        animals[animal]
    );
}

// Preserve sample output for 32-bit IDs; fold the upper CSM nibble for 36-bit IDs.
inline std::string csm_petname(std::string_view csm) {
    if (csm.size() != 9) return {};
    uint64_t id = 0;
    auto [end, error] = std::from_chars(csm.data(), csm.data() + csm.size(), id, 16);
    if (error != std::errc{} || end != csm.data() + csm.size()) return {};
    char name[64];
    petname(static_cast<uint32_t>(id) ^ mix32(static_cast<uint32_t>(id >> 32)), name, sizeof(name));
    return name;
}
} // namespace spear::std_t98
