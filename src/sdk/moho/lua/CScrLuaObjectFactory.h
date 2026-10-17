#pragma once

#include <cstdint>

#include "gpg/core/containers/String.h"
#include "legacy/containers/String.h"
#include "legacy/containers/Vector.h"
#include "lua/LuaObject.h"

namespace gpg
{
  class RRef;
}

namespace moho
{
  class CScriptObject;

  /**
   * Address: 0x004D22D0 (FUN_004D22D0, ?SCR_CreateSimpleMetatable@Moho@@YA?AVLuaObject@LuaPlus@@PAVLuaState@3@@Z)
   * Alias:   0x100C3290 (alt lane)
   *
   * Creates a table metatable and sets __index = self.
   */
  LuaPlus::LuaObject SCR_CreateSimpleMetatable(LuaPlus::LuaState* state);

  /**
   * Address: 0x004D3250 (FUN_004D3250, ?SCR_Import@Moho@@YA?AVLuaObject@LuaPlus@@PAVLuaState@3@VStrArg@gpg@@@Z)
   *
   * What it does:
   * Calls global `import(modulePath)` and returns the resulting Lua object
   * while restoring the caller's original Lua stack top.
   */
  [[nodiscard]] LuaPlus::LuaObject SCR_Import(LuaPlus::LuaState* state, gpg::StrArg modulePath);

  /**
   * Imports a Lua module by calling global `import(modulePath)`.
   * Returns nil object on lookup/call failure.
   */
  [[nodiscard]] LuaPlus::LuaObject SCR_ImportLuaModule(LuaPlus::LuaState* state, const char* modulePath);

  /**
   * Reads one table field via raw stack gettable flow.
   * Returns nil object when table/state/field are invalid.
   */
  [[nodiscard]] LuaPlus::LuaObject
  SCR_GetLuaTableField(LuaPlus::LuaState* state, const LuaPlus::LuaObject& tableObj, const char* fieldName);

  /**
   * Reads one table field and throws with explicit messages when table/field
   * are unavailable.
   */
  [[nodiscard]] LuaPlus::LuaObject SCR_GetLuaTableFieldOrThrow(
    const LuaPlus::LuaObject& tableObj,
    const char* fieldName,
    const char* missingTableMessage,
    const char* missingFieldFormat
  );

  /**
   * Address: 0x004D2380 (FUN_004D2380, ?SCR_ExpectArgs@Moho@@YAXPAVLuaState@LuaPlus@@HHPBD@Z)
   *
   * What it does:
   * Validates Lua argument count in `[minArgs, maxArgs]` (or `>= minArgs`
   * when `maxArgs == -1`) and raises LuaState::Error with `helpText` on
   * mismatch.
   */
  void SCR_ExpectArgs(LuaPlus::LuaState* state, int maxArgs, int minArgs, const char* helpText);

  /**
   * Address: 0x004D2F70 (FUN_004D2F70, ?SCR_ToString@Moho@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@ABVLuaObject@LuaPlus@@@Z)
   *
   * What it does:
   * Serializes one Lua object with SCR byte-stream encoding and returns the raw encoded bytes as `msvc8::string`.
   */
  [[nodiscard]] msvc8::string SCR_ToString(const LuaPlus::LuaObject& object);

  /**
   * Address: 0x004D31D0 (FUN_004D31D0, ?SCR_ToBool@Moho@@YA_NABVLuaObject@LuaPlus@@@Z)
   *
   * What it does:
   * Converts one Lua object to bool using Moho script semantics:
   * numeric non-zero is true; case-insensitive string `"true"`, `"on"`, or
   * `"yes"` is true; every other value is false.
   */
  [[nodiscard]] bool SCR_ToBool(const LuaPlus::LuaObject& object);

  /**
   * Address: 0x004D26D0 (FUN_004D26D0, ?SCR_Copy@Moho@@YA?AVLuaObject@LuaPlus@@ABV23@PAVLuaState@3@@Z)
   *
   * What it does:
   * Deep-copies one Lua object into `state`, recursively cloning table keys and
   * values and cloning userdata through reflected `movRef` handlers when
   * crossing root-state boundaries.
   */
  [[nodiscard]] LuaPlus::LuaObject SCR_Copy(const LuaPlus::LuaObject& source, LuaPlus::LuaState* state);

  /**
   * Address: 0x004D23D0 (FUN_004D23D0, ?SCR_QuoteLuaString@Moho@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@VStrArg@gpg@@@Z)
   *
   * What it does:
   * Returns a Lua-quoted/escaped string literal choosing `'` or `"` based on
   * source contents and escaping control/non-printable bytes.
   */
  [[nodiscard]] msvc8::string SCR_QuoteLuaString(gpg::StrArg text);

  /**
   * Address: 0x004CF0B0 (FUN_004CF0B0, ?SCR_RObjectToLuaMerge@Moho@@YAXABVRRef@gpg@@AAVLuaObject@LuaPlus@@@Z)
   *
   * What it does:
   * Recursively merges one reflected source object/reference into an existing
   * Lua object, handling primitive upcasts, pointers, indexed ranges, and named
   * reflected fields.
   */
  void SCR_RObjectToLuaMerge(const gpg::RRef& source, LuaPlus::LuaObject& destination);

  /**
   * Address: 0x004CF4A0 (FUN_004CF4A0, ?SCR_RObjectToLua@Moho@@...)
   *
   * What it does:
   * Builds one Lua object initialized as `nil` and merges a reflected source
   * reference into it.
   */
  [[nodiscard]] LuaPlus::LuaObject SCR_RObjectToLua(const gpg::RRef& source, LuaPlus::LuaState* state);

  /**
   * Address: 0x004CF510 (FUN_004CF510, ?SCR_LuaBuildObject@Moho@@YA_NVLuaObject@LuaPlus@@ABVRRef@gpg@@_N@Z)
   *
   * What it does:
   * Recursively applies one Lua value/table into a reflected destination reference,
   * handling lexical, indexed, enum-flag, and named-field paths with warning logs.
   */
  [[nodiscard]] bool SCR_LuaBuildObject(LuaPlus::LuaObject valueObject, const gpg::RRef& destination, bool ignoreMissingFields);

  /**
   * Address: 0x004D2550 (FUN_004D2550, ?SCR_GetEnum@Moho@@YAXPAVLuaState@LuaPlus@@VStrArg@gpg@@AAVRRef@5@@Z)
   *
   * What it does:
   * Writes one enum lexical value into a reflected destination and, on failure,
   * raises a Lua error listing all valid enum option names.
   */
  void SCR_GetEnum(LuaPlus::LuaState* state, gpg::StrArg enumString, gpg::RRef& ref);

  /**
   * Address: 0x004CDBA0 (FUN_004CDBA0, ?SCR_LuaDoString@Moho@@YA_NPAVLuaState@LuaPlus@@VStrArg@gpg@@PAVLuaObject@3@@Z)
   *
   * What it does:
   * Loads and executes one Lua chunk from `scriptText`, warns on compile/runtime
   * failures, and restores the caller's original Lua stack top.
   */
  [[nodiscard]] bool SCR_LuaDoString(const char* scriptText, LuaPlus::LuaState* state);

  /**
   * Address: 0x004CE020 (FUN_004CE020, ?SCR_LuaDoFile@Moho@@YA_NPAVLuaState@LuaPlus@@VStrArg@gpg@@PAVLuaObject@3@@Z)
   *
   * IDA signature:
   * bool __usercall Moho::SCR_LuaDoFile@<al>(LuaPlus::LuaState *state@<edi>, const char *filename, LuaPlus::LuaObject *env);
   *
   * What it does:
   * Memory-maps one Lua source file, runs it through `lua_load` with the
   * single-shot file loader, optionally swaps in a caller-provided fenv,
   * and executes the resulting chunk. Saved/restored stack top and warns
   * with file context on every failure path.
   */
  [[nodiscard]] bool SCR_LuaDoFile(LuaPlus::LuaState* state, const char* filename, LuaPlus::LuaObject* env);

  /**
   * Address: 0x004CEA20 (FUN_004CEA20, ?SCR_LuaDoScript@Moho@@YA_NPAVLuaState@LuaPlus@@VStrArg@gpg@@PAVLuaObject@3@@Z)
   *
   * What it does:
   * Executes one resolved script file through `func_LuaDoScript`, restores the
   * caller's Lua stack top, and reports exceptions with file-context warnings.
   */
  [[nodiscard]] bool SCR_LuaDoScript(LuaPlus::LuaState* state, gpg::StrArg scriptPath, LuaPlus::LuaObject* outEnvironment);

  /**
   * Address: 0x004CECD0 (FUN_004CECD0, Moho::SCR_LuaDoFileConcat)
   *
   * What it does:
   * Concatenates and executes one ordered Lua file list as a single chunk,
   * optionally setting one table environment before execution.
   */
  void SCR_LuaDoFileConcat(
    LuaPlus::LuaState* state,
    LuaPlus::LuaObject* outEnvironment,
    msvc8::vector<msvc8::string> files
  );

  /**
   * Address: 0x004CE2C0 (FUN_004CE2C0, func_LuaDoScript)
   *
   * What it does:
   * Resolves one script path, appends hook/mod overrides, then executes the
   * concatenated file chain.
   */
  void func_LuaDoScript(LuaPlus::LuaState* state, const char* scriptPath, LuaPlus::LuaObject* outEnvironment);

  /**
   * Address: 0x004CDF60 (FUN_004CDF60, Moho::SCR_AddHookDirectory)
   *
   * What it does:
   * Adds one hook-directory prefix used when resolving doscript hook variants.
   */
  void SCR_AddHookDirectory(const char* hookDirectory);

  class CScrLuaObjectFactory
  {
  public:
    /**
      * Alias of FUN_10015880 (non-canonical helper lane).
     *
     * What it does:
     * Initializes the base factory and assigns a unique cached object index.
     */
    CScrLuaObjectFactory();

    /**
     * Address: 0x100158A0 (FUN_100158A0, ??0CScrLuaObjectFactory@Moho@@QAE@ABV01@@Z)
     *
     * What it does:
     * Copy-constructs base factory state by copying only the cached object index.
     */
    CScrLuaObjectFactory(const CScrLuaObjectFactory& other);

    /**
     * Address: 0x100158C0 (FUN_100158C0, ??4CScrLuaObjectFactory@Moho@@QAEAAV01@ABV01@@Z)
     *
     * What it does:
     * Assigns only the cached object index.
     */
    CScrLuaObjectFactory& operator=(const CScrLuaObjectFactory& other);

  protected:
    /**
     * Helper constructor for already-recovered specializations that allocate
     * their own index externally before base construction.
     */
    explicit CScrLuaObjectFactory(int32_t factoryObjectIndex);

  public:
    /**
     * Helper used by metatable-factory constructors that follow:
     * `this[1] = ++CScrLuaObjectFactory::sNumIds`.
     */
    static int32_t AllocateFactoryObjectIndex();

    /**
     * Recovery helper:
     * Overrides the cached factory index lane on an already-materialized factory object.
     */
    void SetFactoryObjectIndexForRecovery(const int32_t factoryObjectIndex) noexcept
    {
      mFactoryObjectIndex = factoryObjectIndex;
    }

    /**
     * Address: 0x004CCE70 (FUN_004CCE70, FA exe)
     * Address: 0x100BE9E0 (?Get@CScrLuaObjectFactory@Moho@@QAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     *
     * What it does:
     * Returns one cached factory object from global `__factory_objects`,
     * creating/storing a new entry when the indexed slot is nil.
     */
    LuaPlus::LuaObject Get(LuaPlus::LuaState* state);

  protected:
    virtual LuaPlus::LuaObject Create(LuaPlus::LuaState* state) = 0;

  private:
    static int32_t sNumIds;
    int32_t mFactoryObjectIndex;
  };
  static_assert(sizeof(CScrLuaObjectFactory) == 0x8, "CScrLuaObjectFactory must be 0x8");

  template <typename T>
  class CScrLuaMetatableFactory : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance()
    {
      static CScrLuaMetatableFactory sInstance{};
      return sInstance;
    }

  protected:
    /**
     * Address: 0x00632C70 (FUN_00632C70, `T = moho::IAniManipulator`,
     * ?Create@?$CScrLuaMetatableFactory@VIAniManipulator@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     *
     * What it does:
     * Creates the metatable for `T` through `SCR_CreateSimpleMetatable`
     * (0x004D22D0). One out-of-line body per instantiation; the emission
     * above is identified by the slot it occupies in the vtable at
     * 0x00E213E8, which is its only reference. That vtable's identity is real
     * binary evidence rather than an annotation: the image carries the RTTI
     * type descriptor `.?AV?$CScrLuaMetatableFactory@VIAniManipulator@Moho@@@Moho@@`,
     * so the instantiation is named by the data the runtime itself uses.
     *
     * The body carries nothing type-specific -- the metatable name comes from
     * the registration site, not from here -- so every instantiation is the
     * same 35 bytes and differs only in the call's rel32. That is why
     * `/OPT:ICF` did not fold this one into the hand-specialised
     * `CScrLuaMetatableFactory<CAimManipulator>::Create` at 0x00632C40, which
     * is byte-identical apart from that displacement.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override
    {
      return SCR_CreateSimpleMetatable(state);
    }

  private:
    /**
     * Address: 0x00649BB0 (FUN_00649BB0, `T = moho::CStorageManipulator`)
     *
     * What it does:
     * Chains to `CScrLuaObjectFactory`, which claims the next factory-object
     * index, then installs this instantiation's vftable.
     *
     * Seven instructions, and all seven are this: `[0x010A63A8] += 1` is
     * `++CScrLuaObjectFactory::sNumIds`, the store to `[0xF8D768]` is
     * `mFactoryObjectIndex` at +0x04 of the singleton at 0xF8D764, the store
     * of 0xE2307C to `[0xF8D764]` is that object's vptr, and the object's own
     * address comes back in EAX. MSVC folded `this` to the singleton's address
     * because `Instance()` is the only construction site.
     *
     * It has no callers because the `.CRT$XCL` initializer for the singleton
     * (`register_CScrLuaMetatableFactory_CStorageManipulator_Index`,
     * 0x00BD36B0, ManipulatorStartupRegistrations.cpp) inlines it and drops
     * the vptr store -- a namespace-scope object gets its vptr from the
     * initialized data image, so only the counter bump survives to run time.
     * The linker keeps this copy anyway; `Instance()` is what instantiates it,
     * and `func_CreateLuaCStorageManipulator` (CStorageManipulator.cpp) calls
     * `Instance()`.
     *
     * CStorageManipulator.cpp carried this address as a free
     * `startup_CScrLuaMetatableFactory_CStorageManipulator_Index` marked
     * `[[maybe_unused]]` -- a per-type copy of a template member, which is the
     * shape RULE ONE exists to stop. Roughly forty sibling instantiations
     * still carry their own; each belongs on this line as it is touched.
     */
    CScrLuaMetatableFactory() = default;
  };

  template <>
  class CScrLuaMetatableFactory<CScriptObject*> final : public CScrLuaObjectFactory
  {
  public:
    /**
     * Returns the process-global metatable factory instance.
     */
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x100BA690
     * (?Create@?$CScrLuaMetatableFactory@PAVCScriptObject@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    /**
     * Address: 0x100BA630 (FUN_100BA630, ??0?$CScrLuaMetatableFactory@PAVCScriptObject@Moho@@@Moho@@QAE@XZ)
     *
     * What it does:
     * Constructs the CScriptObject metatable factory and assigns its cache index.
     */
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };
  static_assert(
    sizeof(CScrLuaMetatableFactory<CScriptObject*>) == 0x8, "CScrLuaMetatableFactory<CScriptObject*> must be 0x8"
  );

  /**
   * Address: 0x00BC60D0 (FUN_00BC60D0)
   *
   * What it does:
   * Allocates the next Lua metatable-factory object index and stores it in the
   * recovered `CScrLuaMetatableFactory<CScriptObject*>` startup index lane.
   */
  int register_CScrLuaMetatableFactory_CScriptObject_Index();

  // ---------------------------------------------------------------------------------------------------------
  // Specializations that used to live inside one .cpp each. A specialization seen by only one translation
  // unit is an ODR violation: every other unit instantiated the generic template, so `Unit`'s method binders
  // and the `moho.unit_methods` class binder ended up on two different factory objects (all Unit methods nil).
  // They are declared once here, for every unit; the out-of-line members stay in the owning .cpp files.
  // ---------------------------------------------------------------------------------------------------------
  class CollisionBeamEntity;
  class ScriptedDecal;
  class CPlatoon;
  class CBoneEntityManipulator;
  class CBuilderArmManipulator;
  class CFootPlantManipulator;
  class Entity;
  class CRotateManipulator;
  class CThrustManipulator;
  class CLuaWldUIProvider;
  class CUIWorldMesh;
  class CUIWorldView;
  class CUIMapPreview;
  class CMauiControl;
  class CMauiBorder;
  class CMauiBitmap;
  class CMauiCursor;
  class CMauiLuaDragger;
  class CMauiEdit;
  class CMauiScrollbar;
  class CMauiText;
  class Unit;
  class UserUnit;

  template <>
  class CScrLuaMetatableFactory<CollisionBeamEntity> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<ScriptedDecal> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CPlatoon> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CBoneEntityManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CBuilderArmManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CFootPlantManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<Entity> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CRotateManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CThrustManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CLuaWldUIProvider> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CUIWorldMesh> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CUIWorldView> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CUIMapPreview> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiControl> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00783070 (FUN_00783070, Moho::CScrLuaMetatableFactory<Moho::CMauiControl>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiControl`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiBorder> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00786180 (FUN_00786180, Moho::CScrLuaMetatableFactory<Moho::CMauiBorder>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiBorder`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiBitmap> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00783040 (FUN_00783040, Moho::CScrLuaMetatableFactory<Moho::CMauiBitmap>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiBitmap`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiCursor> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x0078D940 (FUN_0078D940, Moho::CScrLuaMetatableFactory<Moho::CMauiCursor>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiCursor`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiLuaDragger> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x0078E660 (FUN_0078E660, Moho::CScrLuaMetatableFactory<Moho::CMauiLuaDragger>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiLuaDragger`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiEdit> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00794E90 (FUN_00794E90, Moho::CScrLuaMetatableFactory<Moho::CMauiEdit>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiEdit`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiScrollbar> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x007A2470 (FUN_007A2470, Moho::CScrLuaMetatableFactory<Moho::CMauiScrollbar>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiScrollbar`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CMauiText> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x007A4250 (FUN_007A4250, Moho::CScrLuaMetatableFactory<Moho::CMauiText>::Create)
     *
     * What it does:
     * Builds one simple Lua metatable object for `CMauiText`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<Unit> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<UserUnit> final : public CScrLuaObjectFactory
  {
  public:
    CScrLuaMetatableFactory();

    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };


  // Second batch: specializations that sat in per-type headers; a unit that did not include that header
  // silently instantiated the generic template (Projectile / Prop / camera / animation methods nil).
  class CAimManipulator;
  class CAiNavigatorImpl;
  class CAiPersonality;
  class CAnimationManipulator;
  class CSlaveManipulator;
  class CSlideManipulator;
  class HSound;
  class CCollisionManipulator;
  class CPathDebugger;
  class MotorFallDown;
  class MotorSinkAway;
  class Prop;
  class CEconomyEvent;
  class CLobby;
  class Projectile;
  class CDecalHandle;
  class CameraImpl;
  class CUnitScriptTask;
  class CPrefetchSet;
  class CDamage;

  template <>
  class CScrLuaMetatableFactory<CAimManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00632C40 (FUN_00632C40)
     * Mangled: ?Create@?$CScrLuaMetatableFactory@VCAimManipulator@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z
     *
     * What it does:
     * Creates the `CAimManipulator` Lua metatable through
     * `SCR_CreateSimpleMetatable`.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CAiNavigatorImpl> final : public CScrLuaObjectFactory
  {
  public:
    /**
     * Address: 0x1001FDE0 (MohoEngine.dll constructor shape)
     */
    CScrLuaMetatableFactory();

    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x005A7310 (FUN_005A7310, ?Create@?$CScrLuaMetatableFactory@VCAiNavigatorImpl@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CAiPersonality> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x005B9620 (FUN_005B9620, ?Create@?$CScrLuaMetatableFactory@VCAiPersonality@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CAnimationManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00641E10 (FUN_00641E10, ?Create@?$CScrLuaMetatableFactory@VCAnimationManipulator@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     *
     * What it does:
     * Builds the metatable object used for `CAnimationManipulator` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CSlaveManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00646610 (FUN_00646610, ?Create@?$CScrLuaMetatableFactory@VCSlaveManipulator@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     *
     * What it does:
     * Builds the metatable object used for `CSlaveManipulator` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CSlideManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00648420 (FUN_00648420, ?Create@?$CScrLuaMetatableFactory@VCSlideManipulator@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     *
     * What it does:
     * Builds the metatable object used for `CSlideManipulator` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<HSound> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x004E1F20 (FUN_004E1F20, Moho::CScrLuaMetatableFactory<Moho::HSound>::Create)
     *
     * What it does:
     * Builds the default metatable for `HSound` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CCollisionManipulator> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();
    explicit CScrLuaMetatableFactory(std::int32_t factoryObjectIndex);

  protected:
    /**
     * Address: 0x00638640 (FUN_00638640)
     *
     * What it does:
     * Builds the metatable used for `CCollisionManipulator` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;
  };

  template <>
  class CScrLuaMetatableFactory<CPathDebugger> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<MotorFallDown> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00695B90 (FUN_00695B90, Moho::CScrLuaMetatableFactory<Moho::MotorFallDown>::Create)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<MotorSinkAway> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00696FD0 (FUN_00696FD0, Moho::CScrLuaMetatableFactory<Moho::MotorSinkAway>::Create)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<Prop> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00680010 (FUN_00680010, Moho::CScrLuaMetatableFactory<Moho::Prop>::Create)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CEconomyEvent> final : public CScrLuaObjectFactory
  {
  public:
    /**
     * Address: 0x1001FDE0 (MohoEngine.dll constructor shape)
     */
    CScrLuaMetatableFactory();

    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00775B80 (FUN_00775B80)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CLobby> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<Projectile> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x0067FFE0 (FUN_0067FFE0, Moho::CScrLuaMetatableFactory<Moho::Projectile>::Create)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CDecalHandle> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CameraImpl> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]]
    static CScrLuaMetatableFactory& Instance();

  protected:
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CUnitScriptTask> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00623B50 (FUN_00623B50, ?Create@?$CScrLuaMetatableFactory@VCUnitScriptTask@Moho@@@Moho@@MAE?AVLuaObject@LuaPlus@@PAVLuaState@4@@Z)
     *
     * What it does:
     * Builds the metatable object used for `CUnitScriptTask` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CPrefetchSet> final : public CScrLuaObjectFactory
  {
  public:
    [[nodiscard]] static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x004A5FC0 (FUN_004A5FC0, Moho::CScrLuaMetatableFactory<Moho::CPrefetchSet>::Create)
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    static CScrLuaMetatableFactory sInstance;
  };

  template <>
  class CScrLuaMetatableFactory<CDamage> final : public CScrLuaObjectFactory
  {
  public:
    static CScrLuaMetatableFactory& Instance();

  protected:
    /**
     * Address: 0x00739CF0 (FUN_00739CF0, Moho::CScrLuaMetatableFactory<Moho::CDamage>::Create)
     *
     * What it does:
     * Builds the metatable object used for `CDamage` Lua userdata.
     */
    LuaPlus::LuaObject Create(LuaPlus::LuaState* state) override;

  private:
    CScrLuaMetatableFactory();
    static CScrLuaMetatableFactory sInstance;
  };

} // namespace moho
