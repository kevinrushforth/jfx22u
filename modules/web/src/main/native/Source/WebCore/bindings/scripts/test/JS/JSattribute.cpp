/*
    This file is part of the WebKit open source project.
    This file has been generated by generate-bindings.pl. DO NOT MODIFY!

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "config.h"
#include "JSattribute.h"

#include "JSDOMBinding.h"
#include "ScriptExecutionContext.h"
#include "URL.h"
#include "attribute.h"
#include <runtime/JSString.h>
#include <wtf/GetPtr.h>

using namespace JSC;

namespace WebCore {

// Attributes

JSC::EncodedJSValue jsattributeReadonly(JSC::ExecState*, JSC::JSObject*, JSC::EncodedJSValue, JSC::PropertyName);
JSC::EncodedJSValue jsattributeConstructor(JSC::ExecState*, JSC::JSObject*, JSC::EncodedJSValue, JSC::PropertyName);

class JSattributePrototype : public JSC::JSNonFinalObject {
public:
    typedef JSC::JSNonFinalObject Base;
    static JSattributePrototype* create(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::Structure* structure)
    {
        JSattributePrototype* ptr = new (NotNull, JSC::allocateCell<JSattributePrototype>(vm.heap)) JSattributePrototype(vm, globalObject, structure);
        ptr->finishCreation(vm);
        return ptr;
    }

    DECLARE_INFO;
    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::JSValue prototype)
    {
        return JSC::Structure::create(vm, globalObject, prototype, JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }

private:
    JSattributePrototype(JSC::VM& vm, JSC::JSGlobalObject*, JSC::Structure* structure)
        : JSC::JSNonFinalObject(vm, structure)
    {
    }

    void finishCreation(JSC::VM&);
};

class JSattributeConstructor : public DOMConstructorObject {
private:
    JSattributeConstructor(JSC::Structure*, JSDOMGlobalObject*);
    void finishCreation(JSC::VM&, JSDOMGlobalObject*);

public:
    typedef DOMConstructorObject Base;
    static JSattributeConstructor* create(JSC::VM& vm, JSC::Structure* structure, JSDOMGlobalObject* globalObject)
    {
        JSattributeConstructor* ptr = new (NotNull, JSC::allocateCell<JSattributeConstructor>(vm.heap)) JSattributeConstructor(structure, globalObject);
        ptr->finishCreation(vm, globalObject);
        return ptr;
    }

    DECLARE_INFO;
    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::JSValue prototype)
    {
        return JSC::Structure::create(vm, globalObject, prototype, JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }
};

const ClassInfo JSattributeConstructor::s_info = { "attributeConstructor", &Base::s_info, 0, CREATE_METHOD_TABLE(JSattributeConstructor) };

JSattributeConstructor::JSattributeConstructor(Structure* structure, JSDOMGlobalObject* globalObject)
    : DOMConstructorObject(structure, globalObject)
{
}

void JSattributeConstructor::finishCreation(VM& vm, JSDOMGlobalObject* globalObject)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    putDirect(vm, vm.propertyNames->prototype, JSattribute::getPrototype(vm, globalObject), DontDelete | ReadOnly);
    putDirect(vm, vm.propertyNames->length, jsNumber(0), ReadOnly | DontDelete | DontEnum);
}

/* Hash table for prototype */

static const HashTableValue JSattributePrototypeTableValues[] =
{
    { "constructor", DontEnum | ReadOnly, NoIntrinsic, (intptr_t)static_cast<PropertySlot::GetValueFunc>(jsattributeConstructor), (intptr_t) static_cast<PutPropertySlot::PutValueFunc>(0) },
    { "readonly", DontDelete | ReadOnly | CustomAccessor, NoIntrinsic, (intptr_t)static_cast<PropertySlot::GetValueFunc>(jsattributeReadonly), (intptr_t) static_cast<PutPropertySlot::PutValueFunc>(0) },
};

const ClassInfo JSattributePrototype::s_info = { "attributePrototype", &Base::s_info, 0, CREATE_METHOD_TABLE(JSattributePrototype) };

void JSattributePrototype::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    reifyStaticProperties(vm, JSattributePrototypeTableValues, *this);
}

const ClassInfo JSattribute::s_info = { "attribute", &Base::s_info, 0, CREATE_METHOD_TABLE(JSattribute) };

JSattribute::JSattribute(Structure* structure, JSDOMGlobalObject* globalObject, Ref<attribute>&& impl)
    : JSDOMWrapper(structure, globalObject)
    , m_impl(&impl.leakRef())
{
}

JSObject* JSattribute::createPrototype(VM& vm, JSGlobalObject* globalObject)
{
    return JSattributePrototype::create(vm, globalObject, JSattributePrototype::createStructure(vm, globalObject, globalObject->errorPrototype()));
}

JSObject* JSattribute::getPrototype(VM& vm, JSGlobalObject* globalObject)
{
    return getDOMPrototype<JSattribute>(vm, globalObject);
}

void JSattribute::destroy(JSC::JSCell* cell)
{
    JSattribute* thisObject = static_cast<JSattribute*>(cell);
    thisObject->JSattribute::~JSattribute();
}

JSattribute::~JSattribute()
{
    releaseImpl();
}

EncodedJSValue jsattributeReadonly(ExecState* exec, JSObject* slotBase, EncodedJSValue thisValue, PropertyName)
{
    UNUSED_PARAM(exec);
    UNUSED_PARAM(slotBase);
    UNUSED_PARAM(thisValue);
    JSattribute* castedThis = jsDynamicCast<JSattribute*>(JSValue::decode(thisValue));
    if (UNLIKELY(!castedThis)) {
        if (jsDynamicCast<JSattributePrototype*>(slotBase))
            return reportDeprecatedGetterError(*exec, "attribute", "readonly");
        return throwGetterTypeError(*exec, "attribute", "readonly");
    }
    auto& impl = castedThis->impl();
    JSValue result = jsStringWithCache(exec, impl.readonly());
    return JSValue::encode(result);
}


EncodedJSValue jsattributeConstructor(ExecState* exec, JSObject* baseValue, EncodedJSValue, PropertyName)
{
    JSattributePrototype* domObject = jsDynamicCast<JSattributePrototype*>(baseValue);
    if (!domObject)
        return throwVMTypeError(exec);
    return JSValue::encode(JSattribute::getConstructor(exec->vm(), domObject->globalObject()));
}

JSValue JSattribute::getConstructor(VM& vm, JSGlobalObject* globalObject)
{
    return getDOMConstructor<JSattributeConstructor>(vm, jsCast<JSDOMGlobalObject*>(globalObject));
}

bool JSattributeOwner::isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown> handle, void*, SlotVisitor& visitor)
{
    UNUSED_PARAM(handle);
    UNUSED_PARAM(visitor);
    return false;
}

void JSattributeOwner::finalize(JSC::Handle<JSC::Unknown> handle, void* context)
{
    auto* jsattribute = jsCast<JSattribute*>(handle.slot()->asCell());
    auto& world = *static_cast<DOMWrapperWorld*>(context);
    uncacheWrapper(world, &jsattribute->impl(), jsattribute);
}

#if ENABLE(BINDING_INTEGRITY)
#if PLATFORM(WIN)
#pragma warning(disable: 4483)
extern "C" { extern void (*const __identifier("??_7attribute@WebCore@@6B@")[])(); }
#else
extern "C" { extern void* _ZTVN7WebCore9attributeE[]; }
#endif
#endif
JSC::JSValue toJS(JSC::ExecState*, JSDOMGlobalObject* globalObject, attribute* impl)
{
    if (!impl)
        return jsNull();
    if (JSValue result = getExistingWrapper<JSattribute>(globalObject, impl))
        return result;

#if ENABLE(BINDING_INTEGRITY)
    void* actualVTablePointer = *(reinterpret_cast<void**>(impl));
#if PLATFORM(WIN)
    void* expectedVTablePointer = reinterpret_cast<void*>(__identifier("??_7attribute@WebCore@@6B@"));
#else
    void* expectedVTablePointer = &_ZTVN7WebCore9attributeE[2];
#if COMPILER(CLANG)
    // If this fails attribute does not have a vtable, so you need to add the
    // ImplementationLacksVTable attribute to the interface definition
    COMPILE_ASSERT(__is_polymorphic(attribute), attribute_is_not_polymorphic);
#endif
#endif
    // If you hit this assertion you either have a use after free bug, or
    // attribute has subclasses. If attribute has subclasses that get passed
    // to toJS() we currently require attribute you to opt out of binding hardening
    // by adding the SkipVTableValidation attribute to the interface IDL definition
    RELEASE_ASSERT(actualVTablePointer == expectedVTablePointer);
#endif
    return createNewWrapper<JSattribute>(globalObject, impl);
}

attribute* JSattribute::toWrapped(JSC::JSValue value)
{
    if (auto* wrapper = jsDynamicCast<JSattribute*>(value))
        return &wrapper->impl();
    return nullptr;
}

}
