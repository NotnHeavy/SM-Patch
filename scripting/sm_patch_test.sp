//////////////////////////////////////////////////////////////////////////////
// MADE BY NOTNHEAVY. USES GPL-3, AS PER REQUEST OF SOURCEMOD               //
//////////////////////////////////////////////////////////////////////////////

#include <sourcemod>
#include <sdktools>

#include "sm_patch"

static MemoryPatch CTFFlameManager__GetFlameDamageScale__RemoveFlameDensity;
static MemoryPatch CTFProjectile_EnergyRing__Create__SetSpeed;

static int64_t ptr;

public void OnPluginStart()
{
    GameData config = new GameData("sm_patch_test");
    if (!config)
        SetFailState("fail");

    CTFFlameManager__GetFlameDamageScale__RemoveFlameDensity = new MemoryPatch(config, "CTFFlameManager::GetFlameDamageScale()::RemoveFlameDensity", "CTFFlameManager::GetFlameDamageScale()::RemoveFlameDensity::Verify");
    if (!CTFFlameManager__GetFlameDamageScale__RemoveFlameDensity)
        SetFailState("hahahahahhaa \"CTFFlameManager::GetFlameDamageScale()::RemoveFlameDensity\" memory patch failed Hahahahhaahh");
    CTFFlameManager__GetFlameDamageScale__RemoveFlameDensity.Enable();
    
    CTFProjectile_EnergyRing__Create__SetSpeed = new MemoryPatch(config, "CTFProjectile_EnergyRing::Create()::SetSpeed");
    if (!CTFProjectile_EnergyRing__Create__SetSpeed)
        SetFailState("hahahahahahahahha \"CTFProjectile_EnergyRing::Create()::SetSpeed\" memory patch failed HahahAHAH");
    CTFProjectile_EnergyRing__Create__SetSpeed.WriteVariable(0, 3000.00, NumberType_Int32);
    CTFProjectile_EnergyRing__Create__SetSpeed.Enable();
    CTFProjectile_EnergyRing__Create__SetSpeed.WriteVariable(0, -270.00, NumberType_Int32);

    // Try doing it manually too.
    // (x86-32 only)
    if (MemoryPatch.GetPointerSize() == 4)
    {
        int64_t new_speed = { view_as<int>(10.00), 0 };
        any address[2];
        ptr = Malloc64(4);
        address[0] = ptr.low;
        address[1] = ptr.high;
        StoreToAddress64(ptr, NumberType_Int32, new_speed, false);
        CTFProjectile_EnergyRing__Create__SetSpeed.WriteAddress(0, address);
    }

    delete config;
}

public void OnPluginEnd()
{
    CTFFlameManager__GetFlameDamageScale__RemoveFlameDensity.Delete();
    CTFProjectile_EnergyRing__Create__SetSpeed.Delete();
    Free64(ptr);
}