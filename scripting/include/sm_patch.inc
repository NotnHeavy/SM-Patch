//////////////////////////////////////////////////////////////////////////////
// MADE BY NOTNHEAVY. USES GPL-3, AS PER REQUEST OF SOURCEMOD               //
//////////////////////////////////////////////////////////////////////////////

#if defined _sm_patch_included__
    #endinput
#endif
#define _sm_patch_included__

#define SM_PATCH_MAJOR                      1
#define SM_PATCH_MINOR                      0
#define SM_PATCH_REVISION                   1

#include <sourcemod>

#include "smaddress64.inc"

#define PATCH_MAX_BYTES                     64
#define PATCH_BYTE_LENGTH                   2
#define PATCH_HEXSTRING_DELIMITER           "\\x"
#define PATCH_HEXSTRING_DELIMITER_LENGTH    2

#define WILDCARD_BYTE                       0x2A
#define WILDCARD_RIPOFFSET_BYTE             0x3A
#define WILDCARD_END_BYTE                   0x01

#define VARIABLE_COUNT                      8
#define VARIABLE_NAME                       32
#define HEXSTRING_LENGTH                    ((PATCH_BYTE_LENGTH + PATCH_HEXSTRING_DELIMITER_LENGTH) * PATCH_MAX_BYTES + 1)  // \x00 * 64 + NUL
#define VARIABLE_LENGTH                     (VARIABLE_NAME + 3 + HEXSTRING_LENGTH * 2 + 1 + 1)                              // varname = \x00 * 64 (* 2 for wildcard) + ; + space
#define BUFFER_LENGTH                       (VARIABLE_LENGTH * VARIABLE_COUNT + 1)                                          // variable * VARIABLE_COUNT + NUL

//////////////////////////////////////////////////////////////////////////////
// TRAMPOLINE                                                               //
//////////////////////////////////////////////////////////////////////////////

// int3 (TEMPORARY)
// push rax
// movabs rax, (address here)
// jmp rax
// pop rax
static const char jmp_code[]                    = "\x50\x48\xB8\x00\x00\x00\x00\x00\x00\x00\x00\xFF\xE0\x58";
static const int jmp_code_address               = 0x3;

// pop rax
static const char trampoline_prologue[]         = "\x58";

// push rax
// movabs rax, (address here)
// jmp rax
static const char trampoline_epilogue[]         = "\x50\x48\xB8\x00\x00\x00\x00\x00\x00\x00\x00\xFF\xE0"
static const int trampoline_epilogue_address    = 0x3;

// The trampoline body itself should be:
// pop rax
// *original instruction*, [rip+0xDEADBEEF]
// *copied instructions*
// push rax
// movabs rax, (address here)
// jmp rax
// (anything from here should be the desired value)

//////////////////////////////////////////////////////////////////////////////
// MEMORYPATCH OBJECT                                                       //
//////////////////////////////////////////////////////////////////////////////
 
// Internal object representing an array of hexstring bytes.
enum struct bytes_t
{
    int count;
    int buffer[PATCH_MAX_BYTES];
}

// Internal object for a wildcard index.
enum struct wildcard_t
{
    int index;
    int length;
    int instruction_length;
    int64_t value;  // This can either be just the cached variable itself (x86-32), 
                    // or a trampoline and the cached variable afterwards (x86-64).
}

// Internal offsets for the MemoryPatch object.
enum
{
    OFFSET_MEMORYPATCH_ADDRESS          = 0,    // int64_t Address;
    OFFSET_MEMORYPATCH_ENABLED          = 2,    // bool Enabled;
    OFFSET_MEMORYPATCH_PERMSSET         = 3,    // bool PermsSet;
    OFFSET_MEMORYPATCH_BYTES            = 4,    // ArrayList Bytes;
    OFFSET_MEMORYPATCH_WILDCARDOFFSETS  = 5,    // ArrayList WildcardOffsets;
    OFFSET_MEMORYPATCH_ORIGINALBYTES    = 6,    // ArrayList OriginalBytes;
};

// Internal offsets for the wildcard_t object.
enum
{
    OFFSET_WILDCARD_T_INDEX                 = 0,    // int index;
    OFFSET_WILDCARD_T_LENGTH                = 1,    // int length;
    OFFSET_WILDCARD_T_INSTRUCTION_LENGTH    = 2,    // int instruction_length;
    OFFSET_WILDCARD_T_VALUE                 = 3,    // int64_t value;
}

// Allows the programmer to memory patch a sequence of bytes in memory.
// This must be cleared up and disabled manually on plugin end.
//
// This currently uses SM-Address 64 and will use MemoryPointer in the future.
methodmap MemoryPatch < ArrayList
{
    /**
     * Create a new patch from existing gamedata.
     * 
     * @param config        A gamedata handle.
     * @param name          The name of the patch.
     * @param verify        The name of the verification hexstring that is compared
     *                      with the original bytes. Leave empty to ignore.
     * 
     * @return              A MemoryPatch object if this succeeded, or null if
     *                      validation failed.
     * @error               A MemoryPatch object could not be created due to missing
     *                      section data.
     */
    public MemoryPatch(GameData config, const char[] name, const char[] verify = "")
    {
        // Check if the game data is valid.
        if (!config)
        {
            LogError("MemoryPatch requires a valid game data handle.");
            return null;
        }

        // Check if the memory patch name is valid.
        Address address = config.GetAddress(name);
        if (address == Address_Null)
        {
            LogError("Patch name \"%s\" is invalid - check your \"Addresses\" section. If your config is correct, you may have to restart your server due to pseudo-addressing complexities.", name);
            return null;
        }
        int64_t int64;
        int64 = FromPseudoAddress(address);

        // Read from the config into a buffer.
        char buffer[BUFFER_LENGTH];
        if (!config.GetKeyValue(name, buffer, sizeof(buffer)))
        {
            LogError("Patch name \"%s\" is invalid - check your \"Keys\" section.", name);
            return null;
        }

        // Split all of the variables into separate buffers.
        char variables[VARIABLE_COUNT + 1][VARIABLE_LENGTH]; // The last entry is empty.
        int var_count = ExplodeString(buffer, ";", variables, sizeof(variables), sizeof(variables[]), true);
        if (var_count == 1)
        {
            LogError("Patch \"%s\" contains no variables.", name);
            return null;
        }
        
        // Walk through each variable and split it into lvalue/rvalue pairs.
        StringMap map = new StringMap();
        for (int i = 0; i < var_count; ++i)
        {
            // If this entry is empty, ignore it.
            if (strlen(variables[i]) == 0)
                continue;

            // Split each variable buffer.
            char pair[3][VARIABLE_LENGTH]; // Added additional slot to catch out invalid lvalue/rvalue pair lengths.
            if (ExplodeString(variables[i], "=", pair, sizeof(pair), sizeof(pair[]), true) != 2)
            {
                delete map;
                LogError("Patch \"%s\" contains a malformed variable assignment.", name);
                return null;
            }

            // Validate the length of the variable name.
            TrimString(pair[0]);
            TrimString(pair[1]);
            if (strlen(pair[0]) > VARIABLE_NAME)
            {
                delete map;
                LogError("Patch \"%s\" contains an assignment at variable \"%s\" which is longer than %i characters.", name, pair[0], VARIABLE_NAME);
                return null;
            }

            // Explode the hexstring.
            bytes_t bytes;
            char bytes_buffer[PATCH_MAX_BYTES][PATCH_BYTE_LENGTH + 1];
            bytes.count = ExplodeString(pair[1], PATCH_HEXSTRING_DELIMITER, bytes_buffer, sizeof(bytes_buffer), sizeof(bytes_buffer[])) - 1;
            for (int i2 = 0; i2 < bytes.count; ++i2)
                bytes.buffer[i2] = StringToInt(bytes_buffer[i2 + 1], 16);

            // Assign to the created hashmap.
            map.SetArray(pair[0], bytes, sizeof(bytes));
        }

        // Read the patch from the config.
        bytes_t bytes;
        if (!map.GetArray("patch", bytes, sizeof(bytes)))
        {
            delete map;
            LogError("Patch \"%s\" is missing a \"patch\" field.", name);
            return null;
        }

        // Read the verification hexstring from the config.
        bytes_t verify_bytes;
        map.GetArray("verify", verify_bytes, sizeof(verify_bytes));

        // Read the wildcard hexstring from the config.
        bytes_t wildcard_bytes;
        map.GetArray("wildcard", wildcard_bytes, sizeof(wildcard_bytes));

        // Delete the hashmap as we have now obtained all necessary information.
        delete map;

        // Create a new ArrayList object and write basic information to it.
        ArrayList patch = new ArrayList()
        patch.Push(int64.low);
        patch.Push(int64.high);
        patch.Push(false);
        patch.Push(false);

        // Create multiple other ArrayList objects for:
        // - The new bytes used for the patch.
        // - Each wildcard block index.
        // - The original bytes.
        ArrayList bytes_list = new ArrayList();
        ArrayList wildcard_offsets = new ArrayList(sizeof(wildcard_t));
        ArrayList original_bytes = new ArrayList();
        patch.Push(bytes_list);
        patch.Push(wildcard_offsets);
        patch.Push(original_bytes);

        // Store the bytes of the patch. Store indexes of any address-length
        // blocks of wildcards.
        for (int i = 0; i < bytes.count; ++i)
            bytes_list.Push(bytes.buffer[i]);

        // Store the indexes of any address-length blocks of wildcards.
        int negate = 0;
        for (int i = 0; i < wildcard_bytes.count; ++i)
        {
            // Check if this is a negate opcode.
            if (wildcard_bytes.buffer[i] == WILDCARD_END_BYTE)
            {
                ++negate;
                continue;
            }

            // Check if this is a block of wildcards starting with a RIP offset byte.
            wildcard_t index;
            int count = 0;
            index.index = i - negate;
            if (wildcard_bytes.buffer[i] == WILDCARD_RIPOFFSET_BYTE)
            {
                while (i < wildcard_bytes.count && wildcard_bytes.buffer[i] == WILDCARD_RIPOFFSET_BYTE)
                {
                    ++count;
                    ++i;
                }
                index.instruction_length = count;
            }

            // Check to see if this is a block of wildcards.
            if (wildcard_bytes.buffer[i] == WILDCARD_BYTE)
            {
                // Count the number of wildcard bytes.
                while (i < wildcard_bytes.count && wildcard_bytes.buffer[i] == WILDCARD_BYTE)
                {
                    ++count;
                    ++i;
                }

                // Push the wildcard object.
                index.length = count;
                wildcard_offsets.PushArray(index, sizeof(index));
                --i;
            }
            else if (index.instruction_length)
            {
                delete bytes_list;
                delete wildcard_offsets;
                delete original_bytes;
                ThrowError("Patch \"%s\" contains a malformed wildcard block: no wildcard bytes (0x2A) succeed the RIP-offset bytes (0x3A).", name);
                return null;
            }
        }

        // Store the original bytes.
        int64_t value;
        for (int i = 0; i < ((bytes.count > verify_bytes.count) ? bytes.count : verify_bytes.count); ++i)
        {
            // Read from the subroutine and store it to this object.
            value = LoadFromAddress64(int64, NumberType_Int8, i);
            if (i < bytes.count)
                original_bytes.Push(value.low);

            // Compare the read byte with the current verify byte.
            if (i < verify_bytes.count && value.low != verify_bytes.buffer[i] && verify_bytes.buffer[i] != 0x2A)
            {
                delete patch;
                delete bytes_list;
                delete wildcard_offsets;
                delete original_bytes;
                return null;
            }
        }

        // Return.
        return view_as<MemoryPatch>(patch);
    }

    // Is this patch enabled?
    property bool Enabled
    {
        public get() { return this.Get(OFFSET_MEMORYPATCH_ENABLED); }
    }

    /**
     * Write a variable to this memory patch. This can be used to update existing
     * variables.
     * 
     * @param index     The wildcard block index.
     * @param variable  This variable will be copied over to this object and its
     *                  address will be written to the wildcard block.
     * @param size      The size of the variable.
     * @param createNew Re-allocate the memory internally if the chosen index has already
     *                  been written to. This is required if you are writing a block of
     *                  memory with a different size.
     * 
     * @error           Incorrect NumberType value, index is out of bounds or 
     *                  could not allocate memory.
     */
    public void WriteVariable(int index, any variable, NumberType size, bool createNew = false)
    {
        // Error specifically if this is used with NumberType_Int64.
        if (size == NumberType_Int64)
        {
            ThrowError("Please use MemoryPatch::WriteVariable64() with NumberType_Int64.");
            return;
        }

        // Call MemoryPatch::WriteVariable64().
        any buffer[2] = { 0, ... };
        buffer[0] = variable;
        this.WriteVariable64(index, buffer, size, createNew);
    }

    /**
     * Write a variable to this memory patch. This can be used to update existing
     * variables.
     * 
     * @param index     The wildcard block index.
     * @param variable  This variable will be copied over to this object and its
     *                  address will be written to the wildcard block.
     * @param size      The size of the variable.
     * @param createNew Re-allocate the memory internally if the chosen index has already
     *                  been written to. This is required if you are writing a block of
     *                  memory with a different size.
     * 
     * @error           Incorrect NumberType value, index is out of bounds or 
     *                  could not allocate memory.
     */
    public void WriteVariable64(int index, any variable[2], NumberType size, bool createNew = false)
    {
        // Validate the index.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        ArrayList original_bytes = this.Get(OFFSET_MEMORYPATCH_ORIGINALBYTES);
        ArrayList bytes = this.Get(OFFSET_MEMORYPATCH_BYTES);
        if (!(0 <= index < wildcard_offsets.Length))
            ThrowError("%i is out of bounds.", index);

        // Check if the platform is x86-64.
        wildcard_t wildcard;
        wildcard_offsets.GetArray(index, wildcard);
        if (GetPointerSize() == 8)
        {
            // This is going to be horrible. AlliedModders programmers already hate me.
            // We need to allocate a new subroutine which will work as a trampoline. This is 
            // because we cannot just directly write the address of our new variable. This is 
            // fine on x86-32, however in x86-64, instructions work by using relative addresses 
            // to pointers instead. The idea is that this trampoline will actually write the
            // variable and we will patch out some bytes in the original code that will jump
            // to this function. The variable will be located just after the end of this
            // subroutine.
            
            // Before doing anything, check if we already wrote a trampoline function.
            int64_t value;
            int trampoline_size = (sizeof(trampoline_prologue) - 1) + (sizeof(trampoline_epilogue) - 1) + wildcard.length;
            if ((wildcard.value.low != 0 || wildcard.value.high != 0) && !createNew)
            {
                value.low = variable[0];
                value.high = variable[1];
                wildcard.value.Add(trampoline_size);
                StoreToAddress64(wildcard.value, size, value);
                return;
            }
            Free64(wildcard.value);

            // First, start off by configuring some variables. patch_address will be the address
            // of the patch to jump back to from the trampoline.
            int64_t trampoline, patch_address;
            int byte_count = GetNumberTypeSize(size);
            int alloc_size = trampoline_size + byte_count;
            int i = 0;
            patch_address.low = this.Get(OFFSET_MEMORYPATCH_ADDRESS);
            patch_address.high = this.Get(OFFSET_MEMORYPATCH_ADDRESS + 1);
            patch_address.Add(wildcard.index + (sizeof(jmp_code) - 1) - 1);
            
            // Next, allocate the trampoline function itself and write the prologue.
            trampoline = Malloc64(alloc_size); 
            for (; i < sizeof(trampoline_prologue) - 1; ++i)
            {
                value.low = trampoline_prologue[i];
                StoreToAddress64(trampoline, NumberType_Int8, value, .offset = i);
            }

            // Write the first bytes of the original instruction with the relative address that
            // is to be substituted.
            for (; i < sizeof(trampoline_prologue) - 1 + (wildcard.instruction_length - 4); ++i)
            {
                value.low = original_bytes.Get(i - (sizeof(trampoline_prologue) - 1));
                StoreToAddress64(trampoline, NumberType_Int8, value, .offset = i);
            }

            // Write a new relative address that points to the value located after this subroutine
            // in memory.
            value.low = trampoline_size - wildcard.instruction_length - (sizeof(trampoline_prologue) - 1);
            trampoline.Add(i);
            StoreToAddress64(trampoline, NumberType_Int32, value);
            trampoline.Sub(i);

            // Write any other cached instructions that have been overwritten by the patch that
            // jumps to this trampoline.
            for (i = i + 4; i < sizeof(trampoline_prologue) - 1 + wildcard.length; ++i)
            {
                value.low = original_bytes.Get(i - (sizeof(trampoline_prologue) - 1));
                StoreToAddress64(trampoline, NumberType_Int8, value, .offset = i);
            }

            // Write the epilogue of the trampoline while also updating the address stored in
            // rax to be that of patch_address.
            int old_i = i;
            for (int i2 = 0; i < trampoline_size; ++i)
            {
                value.low = trampoline_epilogue[i2];
                StoreToAddress64(trampoline, NumberType_Int8, value, .offset = i);
                ++i2;
            }
            trampoline.Add(old_i + trampoline_epilogue_address);
            StoreToAddress64(trampoline, NumberType_Int64, patch_address, false);
            trampoline.Sub(old_i + trampoline_epilogue_address);

            // Store the value that we actually want to read from after the subroutine.
            value.low = variable[0];
            value.high = variable[1];
            trampoline.Add(trampoline_size);
            StoreToAddress64(trampoline, size, value, false);
            trampoline.Sub(trampoline_size);

            // Finally, patch the original bytes with the following subroutine:
            //   push rax
            //   movabs rax, $trampoline
            //   jmp rax
            //   pop rax
            // This is INCREDIBLY horrible, but this will actually ensure that the variable
            // is (hopefully) substituted correctly. This will only work under two conditions:
            // 1) There is enough space for 14 bytes for this patch, but there probably should be.
            // 2) There is NO following code that relies on addresses relative to the IP.
            for (i = 0; i < sizeof(jmp_code) - 1; ++i)
                bytes.Set(i + wildcard.index, jmp_code[i]);
            for (i = 0; i < 4; ++i)
                bytes.Set(i + jmp_code_address, trampoline.low >> (i * 8));
            for (i = 0; i < 4; ++i)
                bytes.Set(i + jmp_code_address + 4, trampoline.high >> (i * 8));

            // Store the address of the trampoline function and return.
            wildcard_offsets.Set(index, trampoline.low, OFFSET_WILDCARD_T_VALUE);
            wildcard_offsets.Set(index, trampoline.high, OFFSET_WILDCARD_T_VALUE + 1);
            EnablePatch(this);
            return;
        }

        // Validate the size of this wildcard block.
        if (wildcard.length != GetPointerSize())
            ThrowError("Wildcard %i must be of pointer length (%i).", index, GetPointerSize());

        // Since this is x86-32, just allocate the variable.
        int64_t address, value;
        value.low = variable[0];
        value.high = variable[1];
        address = wildcard.value;
        if (createNew)
            Free64(address);
        int byte_count = GetNumberTypeSize(size, "Please use MemoryPatch::WriteVariable64() with NumberType_Int64.");
        if ((address.low == 0 && address.high == 0) || createNew)
            address = Malloc64(byte_count);
        StoreToAddress64(address, size, value, false);

        // Store it to the wildcard offsets ArrayList and update the patch.
        WriteAddress(this, index, address);
        if (createNew)
            EnablePatch(this);
    }

    /**
     * Write a constant to this memory patch. This can be used to update existing
     * constants.
     * 
     * @param index     The wildcard block index.
     * @param constant  The constant to write.
     * @param size      The size of the variable.
     * 
     * @error           Incorrect NumberType value or index is out of bounds.
     */
    public void WriteConstant(int index, any constant, NumberType size)
    {
        // Validate the index.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        if (!(0 <= index < wildcard_offsets.Length))
            ThrowError("%i is out of bounds.", index);

        // Validate the size of this wildcard block.
        wildcard_t wildcard;
        wildcard_offsets.GetArray(index, wildcard);
        if (wildcard.length != 1 && wildcard.length != 2 && wildcard.length != 4)
            ThrowError("Wildcard %i must be 1, 2 or 4 bytes.", index);

        // Write this constant.
        any buffer[2];
        buffer[0] = constant;
        WriteValue(this, index, buffer, GetNumberTypeSize(size, "Please use MemoryPatch::WriteConstant64() with NumberType_Int64."));

        // Update the patch if it is already enabled.
        if (this.Enabled)
            EnablePatch(this);
    }

    /**
     * Write a 64-bit constant to this memory patch. This can be used to update existing
     * constants.
     * 
     * @param index     The wildcard block index.
     * @param constant  The constant to write.
     * @param size      The size of the variable.
     * 
     * @error           Incorrect NumberType value or index is out of bounds.
     */
    public void WriteConstant64(int index, any constant[2], NumberType size)
    {
        // Validate the index.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        if (!(0 <= index < wildcard_offsets.Length))
            ThrowError("%i is out of bounds.", index);

        // Validate the size of this wildcard block.
        wildcard_t wildcard;
        wildcard_offsets.GetArray(index, wildcard);
        if (wildcard.length != 1 && wildcard.length != 2 && wildcard.length != 4 && wildcard.length != 8)
            ThrowError("Wildcard %i must be 1, 2, 4 or 8 bytes.", index);

        // Write this constant.
        WriteValue(this, index, constant, GetNumberTypeSize(size));

        // Update the patch if it is already enabled.
        if (this.Enabled)
            EnablePatch(this);
    }

    /**
     * Write an address to this memory patch. This can be used to update existing
     * constants.
     * 
     * @param index     The wildcard block index.
     * @param address   The address to write.
     * 
     * @error           The index is out of bounds.
     */
    public void WriteAddress(int index, any address[2])
    {
        // Validate the index.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        if (!(0 <= index < wildcard_offsets.Length))
            ThrowError("%i is out of bounds.", index);

        // Validate the size of this wildcard block.
        wildcard_t wildcard;
        wildcard_offsets.GetArray(index, wildcard);
        if (wildcard.length != GetPointerSize())
            ThrowError("Wildcard %i must be of pointer length (%i).", index, GetPointerSize());

        // Write this address.
        this.WriteConstant64(index, address, ((GetPointerSize() == 8) ? NumberType_Int64 : NumberType_Int32));
    }

    /**
     * Write a buffer of instructions to this memory patch. This can be used to
     * update existing instructions.
     * 
     * @param index     The wildcard block index.
     * @param buffer    The buffer to write.
     * @param size      The length of the buffer.
     * 
     * @error           The index is out of bounds, or the buffer is too long.
     */
    public void WriteBuffer(int index, char[] buffer, int size)
    {
        // Validate the index.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        ArrayList bytes = this.Get(OFFSET_MEMORYPATCH_BYTES);
        if (!(0 <= index < wildcard_offsets.Length))
            ThrowError("%i is out of bounds.", index);

        // Validate the size.
        int offset = wildcard_offsets.Get(index, OFFSET_WILDCARD_T_INDEX);
        if (offset + size >= bytes.Length)
        {
            ThrowError("Buffer of length %i is longer than total patch length %i!", size, bytes.Length);
            return;
        }

        // Write this buffer.
        this.RestoreBytes(index);
        for (int i = 0; i < size; ++i)
            bytes.Set(offset + i, buffer[i]);

        // Update the patch if it is already enabled.
        if (this.Enabled)
            EnablePatch(this);
    }

    /**
     * Restore all original bytes that were replaced at a wildcard block.
     * 
     * @param index     The wldcard block index.
     * 
     * @error           The index is out of bounds.
     */
    public void RestoreBytes(int index)
    {
        // Validate the index.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        ArrayList bytes = this.Get(OFFSET_MEMORYPATCH_BYTES);
        ArrayList original_bytes = this.Get(OFFSET_MEMORYPATCH_ORIGINALBYTES);
        if (!(0 <= index < wildcard_offsets.Length))
            ThrowError("%i is out of bounds.", index);

        // Free the existing cached variable, if found.
        int64_t address;
        address.low = wildcard_offsets.Get(index, OFFSET_WILDCARD_T_VALUE);
        address.high = wildcard_offsets.Get(index, OFFSET_WILDCARD_T_VALUE + 1);
        if (address.low != 0 || address.high != 0)
        {
            Free64(address);
            wildcard_offsets.Set(index, 0, OFFSET_WILDCARD_T_VALUE);
            wildcard_offsets.Set(index, 0, OFFSET_WILDCARD_T_VALUE + 1);
        }

        // Restore the original bytes.
        for (int i = 0, size = wildcard_offsets.Get(index, OFFSET_WILDCARD_T_LENGTH); i < size; ++i)
            bytes.Set(i, original_bytes.Get(i));
    }

    /**
     * Enable this patch.
     * 
     * @return      True if successful, otherwise false.
     */
    public bool Enable()
    {
        // Check if this is already enabled.
        if (this.Enabled)
            return false;

        // Enable this patch.
        return EnablePatch(this);
    }

    /**
     * Disable this patch.
     * 
     * @return      True if successful, otherwise false.
     */
    public bool Disable()
    {
        // Walk through the address set and unpatch each opcode.
        // Here we just set each byet individually as the memory permissions
        // should have already been set.
        ArrayList original_bytes = this.Get(OFFSET_MEMORYPATCH_ORIGINALBYTES);
        int64_t address, value;
        address.low = this.Get(OFFSET_MEMORYPATCH_ADDRESS);
        address.high = this.Get(OFFSET_MEMORYPATCH_ADDRESS + 1);
        for (int i = 0, size = original_bytes.Length; i < size; ++i)
        {
            value.low = original_bytes.Get(i);
            StoreToAddress64(address, NumberType_Int8, value, false, i);
        }

        // Check if this is already disabled.
        if (!this.Enabled)
            return false;

        // Set this patch as disabled and return.
        this.Set(OFFSET_MEMORYPATCH_ENABLED, false);
        return true;
    }

    /**
     * Delete this MemoryPatch object. This will automatically disable this
     * patch.
     */
    public void Delete()
    {
        // Disable this patch.
        this.Disable();

        // Clear up cached variables, if there are any.
        ArrayList wildcard_offsets = this.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
        for (int i = 0, size = wildcard_offsets.Length; i < size; ++i)
        {
            int64_t address;
            address.low = wildcard_offsets.Get(i, OFFSET_WILDCARD_T_VALUE);
            address.high = wildcard_offsets.Get(i, OFFSET_WILDCARD_T_VALUE + 1);
            if (address.low != 0 || address.high != 0)
                Free64(address);
        }

        // Delete each ArrayList object.
        delete view_as<ArrayList>(this.Get(OFFSET_MEMORYPATCH_BYTES));
        delete view_as<ArrayList>(this.Get(OFFSET_MEMORYPATCH_ORIGINALBYTES));
        delete view_as<ArrayList>(this);
        delete wildcard_offsets;
    }
    
    /**
     * @return      True if this platform is Windows, otherwise false.
     */
    public static bool IsWindows()
    {
        // Return the result if it has already been calculated.
        static int result = -1;
        if (result != -1)
            return view_as<bool>(result);

        // Build a path relative to the SourceMod directory to find out the path
        // separator, then assign the result and return it.
        char buffer[PLATFORM_MAX_PATH];
        BuildPath(Path_SM, buffer, sizeof(buffer), "/");
        result = (buffer[strlen(buffer) - 1] == '\\');
        return view_as<bool>(result);
    }

    /**
     * @return      True if this platform is Linux, otherwise false.
     */
    public static bool IsLinux()
    {
        // This is fine as a non-gamedata solution because SourceMod only supports 
        // Windows and Linux.
        return !MemoryPatch.IsWindows();
    }

    /**
     * Return the pointer size of this platform.
     * 
     * @return      4 if this is a 32-bit platform, otherwise 8 if this is a
     *              64-bit platform.
     */
    public static int GetPointerSize()
    {
        return GetPointerSize();
    }
}

//////////////////////////////////////////////////////////////////////////////
// INTERNAL                                                                 //
//////////////////////////////////////////////////////////////////////////////

// Get a byte from the bytes ArrayList.
static int GetByte(MemoryPatch patch, int index)
{
    int byte = view_as<ArrayList>(patch.Get(OFFSET_MEMORYPATCH_BYTES)).Get(index);
    if (byte != 0x2A)
        return byte;
    return view_as<ArrayList>(patch.Get(OFFSET_MEMORYPATCH_ORIGINALBYTES)).Get(index);
}

// Enable a memory patch.
static bool EnablePatch(MemoryPatch patch)
{
    // Walk through the address set and patch each opcode.
    // We first do this by storing chunks of 32-bit ints, because 
    // SetMemAccess() is slow.
    ArrayList bytes = patch.Get(OFFSET_MEMORYPATCH_BYTES);
    int64_t address, value;
    address.low = patch.Get(OFFSET_MEMORYPATCH_ADDRESS);
    address.high = patch.Get(OFFSET_MEMORYPATCH_ADDRESS + 1);
    bool update_mem_access = !patch.Get(OFFSET_MEMORYPATCH_PERMSSET);

    // Write all the patched bytes.
    for (int i = 0, size = bytes.Length; i < size; ++i)
    {
        value.low = GetByte(patch, i);
        StoreToAddress64(address, NumberType_Int8, value, update_mem_access, i);
    }

    // Set this patch as enabled and return.
    patch.Set(OFFSET_MEMORYPATCH_ENABLED, true);
    if (update_mem_access)
        patch.Set(OFFSET_MEMORYPATCH_PERMSSET, true);
    return true;
}

// Write an address to a wildcard block.
static void WriteAddress(MemoryPatch patch, int index, int64_t value, bool writeExtra = false)
{
    ArrayList bytes = patch.Get(OFFSET_MEMORYPATCH_BYTES);
    ArrayList wildcard_offsets = patch.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
    int offset = wildcard_offsets.Get(index, OFFSET_WILDCARD_T_INDEX);
    wildcard_offsets.Set(index, value.low, OFFSET_WILDCARD_T_VALUE);
    wildcard_offsets.Set(index, value.high, OFFSET_WILDCARD_T_VALUE + 1);
    for (int i = 0; i < 4; ++i)
        bytes.Set(offset + i, (value.low >> (i * 8)) & 0xFF);
    if (writeExtra)
    {
        for (int i = 4; i < 8; ++i)
            bytes.Set(offset + i, (value.high >> (i * 8)) & 0xFF);
    }
}

// Write a value to a wildcard block.
static void WriteValue(MemoryPatch patch, int index, any value[2], int size)
{
    // Write the value.
    ArrayList bytes = patch.Get(OFFSET_MEMORYPATCH_BYTES);
    ArrayList wildcard_offsets = patch.Get(OFFSET_MEMORYPATCH_WILDCARDOFFSETS);
    patch.RestoreBytes(index);
    int offset = wildcard_offsets.Get(index, OFFSET_WILDCARD_T_INDEX);
    for (int i = 0; i < ((size == 8) ? 4 : size); ++i)
        bytes.Set(offset + i, (value[0] >> (i * 8)) & 0xFF);
    if (size == 8)
    {
        for (int i = 4; i < 8; ++i)
            bytes.Set(offset + i, (value[1] >> (i * 8)) & 0xFF);
    }
}

// Get the size of a NumberType value in bytes.
static int GetNumberTypeSize(NumberType type, char[] int64_error = "")
{
    // Accept valid NumberType values.
    switch (type)
    {
        case NumberType_Int8:
            return 1;
        case NumberType_Int16:
            return 2;
        case NumberType_Int32:
            return 4;
        case NumberType_Int64:
        {
            if (int64_error[0])
                ThrowError(int64_error);
            return 8;
        }
    }
    
    // Wrong NumberType constant?
    ThrowError("%i is not a recognised NumberType constant.", type);
    return 0;
}