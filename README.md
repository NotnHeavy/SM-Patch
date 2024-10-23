# SM-Patch

Inspired by [Source Scramble](https://github.com/nosoop/SMExt-SourceScramble) and [MemoryPatch](https://github.com/Kenzzer/MemoryPatch) somewhat combined into one, this memory patching include allows you to not only patch bytes, but also provide a verifying mechanism (for when your desired game has an update) and an abstracted interface for patching variables and constants on both 32-bit and 64-bit platforms.

## Installation
You will need to install [SM-Address64](https://github.com/NotnHeavy) in order to allow this include to function correctly. SourceMod should soon have natives for 64-bit address handling (and hopefully for memory allocation as well), however they are not ready yet. The current bets are on [Kenzzer's MemoryPointer](https://github.com/alliedmodders/sourcemod/pull/2196).

Afterwards, you may include `sm_patch.inc` and use it as appropriately.

## Understanding the gamedata
To create a memory patch, you must create a gamedata file and write the following:

### The signature of your function in the `Signatures` section:
```
"CTFFlameManager::GetFlameDamageScale()"
{
    "library"   "server"
    "windows"   "\x55\x8B\xEC\x83\xEC\x18\x8B\x15\x2A\x2A\x2A\x2A\x57"
    "linux"     "@_ZNK15CTFFlameManager19GetFlameDamageScaleEPK10tf_point_tP9CTFPlayer"
    "windows64" "\x4C\x8B\xDC\x55\x56\x48\x81\xEC\xA8\x00\x00\x00"
    "linux64"   "@_ZNK15CTFFlameManager19GetFlameDamageScaleEPK10tf_point_tP9CTFPlayer"
}
```

### The address + offset of your memory patch in the `Addresses` section:
```
"CTFFlameManager::GetFlameDamageScale()::RemoveFlameDensity"
{
    "signature"     "CTFFlameManager::GetFlameDamageScale()"
    "windows"
    {
        "offset"    "390"
    }
    "linux"
    {
        "offset"    "292"
    }
    "windows64"
    {
        "offset"    "429"
    }
    "linux64"
    {
        "offset"    "303"
    }
}
```

### The memory patch information in the `Keys` section:
```
"CTFFlameManager::GetFlameDamageScale()::RemoveFlameDensity"
{
    "windows"   "patch = \x31\xC0\x90; verify = \x66\x3B\xC1\x74\x2A;"
    "linux"     "patch = \x31\xC0\x90\x90; verify = \x66\x83\xF8\xFF\x74\x2A;"
    "windows64"	"patch = \x48\x31\xC0; verify = \x66\x3B\xC1;"
    "linux64"   "patch = \x48\x31\xC0\x90; verify = \x66\x83\xF8\xFF\x74\x2A;"
}
```
Note that you must define the patch using the `patch = ` keyword. You may also define bytes, using the `verify = ` keyword, that are compared with the original subroutine to verify whether they match, to halt the memory patch in cases that they do not match due to a game update. An unsuccessful patch can be handled specifically by the developer. Verify bytes may also include the wildcard byte `\x2A`, which indicates that this byte in memory can be any byte.

You may also specify the `wildcard = ` keyword to indicate wildcard blocks where a variable address or constant may be written to, using the `MemoryPatch::Write*` methods:
```
"CTFProjectile_EnergyRing::Create()::SetSpeed"
{
    "windows"   "patch = \x2A\x2A\x2A\x2A; wildcard = \x2A\x2A\x2A\x2A;"
    "linux"     "patch = \x2A\x2A\x2A\x2A; wildcard = \x2A\x2A\x2A\x2A;"
    "windows64"	"patch = \x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90; wildcard = \x3A\x3A\x3A\x3A\x3A\x3A\x3A\x3A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x01; verify = \xF3\x0F\x10\x15\x2A\x2A\x2A\x2A\x48\x8D\x54\x24\x20\xF3\x0F\x10\x44\x24\x30"
    "linux64"   "patch = \x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90; wildcard = \x3A\x3A\x3A\x3A\x3A\x3A\x3A\x3A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x2A\x2A;"
}
```
Wildcards are defined using the byte `\x2A`, with a variable length. Wildcard blocks are terminated either when the next byte is different, or by using the terminator byte `\x01`.

Writing a constant, address or buffer is typically very easy and you only need to define a wildcard block (although this must be 1, 2, 4 or 8 bytes in length for constants, or 4 or 8 bytes (for 32-bit and 64-bit platforms respectively) for address constants) while either calling the `MemoryPatch::WriteConstant`, `MemoryPatch::WriteConstant64`, `MemoryPatch::WriteAddress` or `MemoryPatch::WriteBuffer` methods.

However, variables are a lot more complicated. The sample above using wildcard blocks is for a variable example. 32-bit platforms are actually much simpler, as you only need to specify a wildcard block that is 4 bytes in length. To write a new variable, use the `MemoryPatch::WriteVariable` or `MemoryPatch::WriteVariable64` methods. One thing to point out is that you can also use wildcard bytes (`\x2A`) in the patch section, meaning if nothing has been written to that byte, the byte used when patching will be the original byte from the subroutine instead.

64-bit platforms are vastly more complicated. These memory patches require at least 14 bytes of memory due to the complex nature of the internal patching. Here, you must highlight the entire instruction that you are replacing the variable address at with the byte `\x3A` instead in the wildcard section. However, x86-64 instructions are almost never 14 bytes and therefore you will have to highlight any following instructions with the normal wildcard byte (`\x2A`), until at least 14 bytes worth of instructions have been covered. Any bytes after the 14th byte must be NOP'd (`\x90`) in the patch section.

The reason for this incredibly complicated patching is because this include's goal is to substitute the variable address of the original instruction with one relative to your designated value elsewhere in memory, but due to the concept of RIP-relative addresses in x86-64, this must be done in a trampoline instead. This include will not rewrite any of the instructions themselves (which will not always fit in the space of your original instruction anyway). However, due to the sacrificing of 14 bytes, this will not work for instructions where the following instruction also accesses relative addresses and may lead to a segfault.

From here, observe the `sm_patch_test.sp` sample file which toggles these basic memory patches.

## Dependencies
- [SourceMod 1.12](https://www.sourcemod.net/downloads.php?branch=1.12) or later.
- [SM-Address64](https://github.com/NotnHeavy).