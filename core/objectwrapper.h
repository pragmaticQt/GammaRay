/*
 *  objecthandle.h
 *
 *  This file is part of GammaRay, the Qt application inspection and
 *  manipulation tool.
 *
 *  Copyright (C) 2018 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
 *  Author: Anton Kreuzkamp <anton.kreuzkamp@kdab.com>
 *
 *  Licensees holding valid commercial KDAB GammaRay licenses may use this file in
 *  accordance with GammaRay Commercial License Agreement provided with the Software.
 *
 *  Contact info@kdab.com if any conditions of this licensing are not clear to you.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GAMMARAY_OBJECTHANDLE_H
#define GAMMARAY_OBJECTHANDLE_H

#include <core/probe.h>
#include <common/objectid.h>
#include <compat/qasconst.h>

#include <QObject>
#include <QMetaObject>
#include <QMetaProperty>
#include <QDebug>
#include <QMetaType>
#include <QMutex>
#include <QSemaphore>
#include <QThread>

#include <core/metaobject.h>
#include <core/metaproperty.h>

#include <utility>
#include <future>
#include <tuple>
#include <memory>
#include <typeindex>

#include <iostream>

#include <list>

#include <private/qobject_p.h>
#include <private/qmetaobject_p.h>

template<typename T> class error;

template<int i> class err;

/**
 * May only be used in cases, where non-constexpr if would still produce valid code
 */
#define IF_CONSTEXPR if

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
class QSemaphoreReleaser
{
public:
    QSemaphoreReleaser() = default;
    explicit QSemaphoreReleaser(QSemaphore &sem) noexcept
        : m_sem(&sem) {}

    ~QSemaphoreReleaser()
    {
        if (m_sem)
            m_sem->release();
    }
private:
    QSemaphore *m_sem = nullptr;
};
#endif



enum ObjectWrapperFlag {
    NoFlags = 0,
    Getter = 1,
    MemberVar = 2,
    DptrGetter = 4,
    DptrMember = 8,
    CustomCommand = 16,

    QProp = 128,
    OwningPointer = 256,
    NonOwningPointer = 512,
    ForeignPointerBit = 1024,
    ForeignPointer = ForeignPointerBit | NonOwningPointer, // ForeignPointer implies NonOwning
    NonConst = 2048,
};


/**
 * Defines a getter function with the name @p FieldName, which returns the data
 * stored at index @p StorageIndex in d->dataStorage or - in the
 * non-caching case - the live value of the property.
 *
 * This is internal for use in other macros.
 */
#define DEFINE_GETTER(FieldName, StorageIndex, Flags) \
decltype(wrap<Flags>(fetch_##FieldName<Flags>(static_cast<value_type*>(nullptr)))) FieldName() const \
{ \
    Q_ASSERT(d_ptr()); \
    d_ptr()->semaphore.acquire(); \
    QSemaphoreReleaser releaser { d_ptr()->semaphore }; \
 \
    IF_CONSTEXPR (cachingDisabled<ThisClass_t>::value) { \
        return wrap<Flags>(fetch_##FieldName<Flags>(object())); \
    } else { \
        return wrapPhase2<Flags>(d_ptr()->cache<value_type>()->get< StorageIndex >()); \
    } \
} \

/**
 * Defines a setter function with the name @p SetterName, which sets the cached
 * value stored at index @p StorageIndex in d->dataStorage and
 * (in the multi-threaded case deferredly) updates the actual property value.
 * In the non-caching case it just sets the live value of the property directly.
 *
 * This is internal for use in other macros.
 */
#define DEFINE_SETTER(FieldName, SetterName, StorageIndex, Flags) \
void SetterName(decltype(wrap<Flags>(fetch_##FieldName<Flags>(static_cast<value_type*>(nullptr)))) newValue) \
{ \
    Q_ASSERT(d_ptr()); \
    d_ptr()->semaphore.acquire(); \
    QSemaphoreReleaser releaser { d_ptr()->semaphore }; \
    \
    IF_CONSTEXPR (cachingDisabled<ThisClass_t>::value) { \
        write_##FieldName<Flags>(object(), unwrap(newValue)); \
    } else { \
        d_ptr()->cache<value_type>()->get< StorageIndex >() = newValue; \
        /* TODO Defer calling the setter */ \
        write_##FieldName<Flags>(object(), unwrap(newValue)); \
    } \
} \


/**
 * Defines a getter function with the name @p FieldName, which returns the data
 * stored at index @p StorageIndex in d->dataStorage or - in the
 * non-caching case - the live value of the property.
 *
 * This is internal for use in other macros.
 */
#define DEFINE_REFRESH_PROPERTY(FieldName, StorageIndex, Flags) \
void refresh_##FieldName() \
{ \
    Q_ASSERT(d_ptr()); \
    IF_CONSTEXPR (cachingDisabled<ThisClass_t>::value) { \
        return; \
    } \
    \
    d_ptr()->semaphore.acquire(); \
    QSemaphoreReleaser releaser { d_ptr()->semaphore }; \
    \
    d_ptr()->cache<value_type>()->get< StorageIndex >() \
        = wrapPhase1<Flags>(fetch_##FieldName<Flags>(object())); \
} \


/**
 * Defines a wrapper function for direct access to the property, abstracting
 * away the different kinds of properties (getter, member variable, custom
 * command).  This differs from the DEFINE_GETTER in that the fetch function
 * never caches things. Instead it's used to update the cache.
 *
 * This is internal for use in other macros.
 */
#define DEFINE_FETCH_FUNCTION_PROP(FieldName) \
template<int Flags, typename T = pimplClass_t<ThisClass_t>, typename std::enable_if<(Flags & DptrGetter) != 0 && (Flags & NonConst) == 0>::type* = nullptr> \
static auto fetch_##FieldName(const value_type *object) \
-> decltype(std::declval<T>().FieldName()) \
{ \
    static_assert(!std::is_same<T, void>::value, "Unknown Private Class: You need to add a PRIVATE_CLASS(...) macro to your wrapper definition."); /*FIXME can we make that assert actually effective instead of SFINAE?*/ \
    return static_cast<const T*>(T::get(object))->FieldName(); \
} \
template<int Flags, typename T = pimplClass_t<ThisClass_t>, typename std::enable_if<(Flags & DptrMember) != 0 && (Flags & NonConst) == 0>::type* = nullptr> \
static auto fetch_##FieldName(const value_type *object) \
-> decltype(std::declval<T>().FieldName) \
{ \
    static_assert(!std::is_same<T, void>::value, "Unknown Private Class: You need to add a PRIVATE_CLASS(...) macro to your wrapper definition."); /*FIXME can we make that assert actually effective instead of SFINAE?*/ \
    return static_cast<const T*>(T::get(object))->FieldName; \
} \
\
template<int Flags, typename T = pimplClass_t<ThisClass_t>, typename std::enable_if<(Flags & DptrGetter) != 0 && (Flags & NonConst) != 0>::type* = nullptr> \
static auto fetch_##FieldName(value_type *object) \
-> decltype(std::declval<T>().FieldName()) \
{ \
    static_assert(!std::is_same<T, void>::value, "Unknown Private Class: You need to add a PRIVATE_CLASS(...) macro to your wrapper definition."); /*FIXME can we make that assert actually effective instead of SFINAE?*/ \
    return static_cast<T*>(T::get(object))->FieldName(); \
} \
template<int Flags, typename T = pimplClass_t<ThisClass_t>, typename std::enable_if<(Flags & DptrMember) != 0 && (Flags & NonConst) != 0>::type* = nullptr> \
static auto fetch_##FieldName( value_type *object) \
-> decltype(std::declval<T>().FieldName) \
{ \
    static_assert(!std::is_same<T, void>::value, "Unknown Private Class: You need to add a PRIVATE_CLASS(...) macro to your wrapper definition."); /*FIXME can we make that assert actually effective instead of SFINAE?*/ \
    return static_cast<T*>(T::get(object))->FieldName; \
} \
\
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & Getter) != 0 && (Flags & NonConst) == 0>::type* = nullptr> \
static auto fetch_##FieldName(const T *object) \
-> decltype(std::declval<const T>().FieldName()) \
{ \
    return object->FieldName(); \
} \
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & Getter) != 0 && (Flags & NonConst) != 0>::type* = nullptr> \
static auto fetch_##FieldName(T *object) \
-> decltype(std::declval<T>().FieldName()) \
{ \
    return object->FieldName(); \
} \
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & MemberVar) != 0>::type* = nullptr> \
static auto fetch_##FieldName(const T *object) \
-> decltype(std::declval<const T &>().FieldName) \
{ \
    return object->FieldName; \
} \

/**
 * Like DEFINE_FETCH_FUNCTION_PROP but with custom expression.
 *
 * This is internal for use in other macros.
 */
#define DEFINE_FETCH_FUNCTION_CUSTOM_EXPRESSION(FieldName, Type, Expr) \
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & NonConst) == 0>::type* = nullptr> \
static auto fetch_##FieldName(const T *object) \
-> Type \
{ \
    return Expr; \
} \
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & NonConst) != 0>::type* = nullptr> \
static auto fetch_##FieldName(T *object) \
-> Type \
{ \
    return Expr; \
} \


/**
 * Defines a wrapper function for direct write access to the property,
 * abstracting away the different methods of writing properties (setter, member
 * variable, custom command).  This differs from the DEFINE_SETTER in that the
 * write function never caches things. Instead it's used to update the cache.
 *
 * This is internal for use in other macros.
 */
#define DEFINE_WRITE_FUNCTION_PROP(FieldName, SetterName) \
template<int Flags, typename T = pimplClass_t<ThisClass_t>, typename std::enable_if<(Flags & DptrGetter) != 0>::type* = nullptr> /*FIXME T must be the private class! */ \
static void write_##FieldName(value_type *object, decltype(std::declval<T>().FieldName()) newVal) \
{ \
    static_cast<T*>(T::get(object))->SetterName(newVal); \
} \
template<int Flags, typename T = pimplClass_t<ThisClass_t>, typename std::enable_if<(Flags & DptrMember) != 0>::type* = nullptr> /*FIXME T must be the private class! */ \
static void write_##FieldName(value_type *object, decltype(std::declval<T>().FieldName) newVal) \
{ \
    static_cast<T*>(T::get(object))->FieldName = newVal; \
} \
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & Getter) != 0>::type* = nullptr> \
static void write_##FieldName(T *object, decltype(std::declval<T>().FieldName()) newVal) \
{ \
    object->SetterName(newVal); \
} \
template<int Flags, typename T = value_type, typename std::enable_if<(Flags & MemberVar) != 0>::type* = nullptr> \
static void write_##FieldName(T *object, decltype(std::declval<T>().FieldName) newVal) \
{ \
    object->FieldName = newVal; \
} \



template<typename T, typename ...Args>
struct tuple_append {};
template<typename T, typename ...TupleArgs_t>
struct tuple_append<std::tuple<TupleArgs_t...>, T> {
    using type = std::tuple<TupleArgs_t..., T>;
};
template<typename Tuple_t, typename T>
using tuple_append_t = typename tuple_append<Tuple_t, T>::type;


/**
 * Defines a method, which holds state using overloading and inheritance tricks
 * as described at https://woboq.com/blog/verdigris-implementation-tricks.html
 *
 * Incrementing the counter is done by using a combination of DATA_APPEND and
 * DEFINE_COUNTER. Use DEFINE_COUNTER to define a constexpr variable holding the
 * current count + 1. Use DATA_APPEND to make it the new current count.
 *
 * Use StateExpression to return arbitary content, possibly using recursion. Eg.
 * you can do
 * `std::tuple_cat(MethodName(self, __counter.prev()), std::make_tuple(...)))`
 * to recursively compose a compile-time list.
 *
 * This is meant for internal use in other macros only.
 */
#define DATA_APPEND(Counter, AppendedType, StateExpr) \
static auto __data(ObjectWrapperPrivate *d, __number<Counter> __counter) \
-> tuple_append_t<decltype(__data(d, __counter.prev())), AppendedType> \
{ \
    return std::tuple_cat(__data(d, __counter.prev()), std::make_tuple(StateExpr)); \
}

/**
 * Defines an overload to __metadata, which adds \p FieldName to the metaobject
 * and recursively calls the previously defined overload of __metadata.
 * (see https://woboq.com/blog/verdigris-implementation-tricks.html).
 *
 * This is meant for internal use in other macros only.
 */
#define ADD_TO_METAOBJECT(FieldName, FieldType, Flags) \
static void __metadata(__number<W_COUNTER_##FieldName>, MetaObject *mo) \
{ \
    mo->addProperty(GammaRay::MetaPropertyFactory::makeProperty(#FieldName, &ThisClass_t::FieldName)); \
    __metadata(__number< W_COUNTER_##FieldName - 1 >{}, mo); \
}

/**
 * Defines a constexpr variable that fetches the current value of the count
 * used by the constexpr-state code defined by STATE_APPEND and stores it
 * incremented by one.
 *
 * @p CounterName is the name of the variable to be defined
 * @p CounterMethod is the name of the method, which holds the state. This
 *                  method must return a tuple, whose size equals the
 *                  current count.
 *
 * Incrementing the counter is done by using a combination of STATE_APPEND and
 * DEFINE_COUNTER. Use DEFINE_COUNTER to define a constexpr variable holding the
 * current count + 1. Use STATE_APPEND to make it the new current count.
 *
 * This is meant for internal use in other macros only.
 */
#define DEFINE_COUNTER(CounterName, CounterMethod) \
static constexpr int CounterName = \
std::tuple_size<decltype(CounterMethod(static_cast<ObjectWrapperPrivate *>(nullptr), __number<255>{}))>::value + 1; \


/**
 * Defines an overload to __connectToUpdates, which connects an update property
 * slot to the notify signal of the property \p FieldName. It then recursively
 * calls the previously defined overload of __connectToUpdates
 * (see https://woboq.com/blog/verdigris-implementation-tricks.html).
 *
 * This is meant for internal use in other macros only.
 */
#define CONNECT_TO_UPDATES(FieldName, Flags) \
static void __connectToUpdates(ObjectWrapperPrivate *d, __number<W_COUNTER_##FieldName>) \
{ \
    d->connectToUpdates< value_type, W_COUNTER_##FieldName - 1, Flags >(&ThisClass_t::fetch_##FieldName<Flags>, #FieldName); \
    __connectToUpdates(d, __number< W_COUNTER_##FieldName - 1 >{}); \
} \


/**
 * Adds a property to the object wrapper. The data will be accessible
 * through a getter in the wrapper, named as \p FieldName.
 *
 * The property can be customized by a couple of \p Flags:
 *  Getter: If this flag is set, data will be fetched using obj->FieldName()
 *  MemberVar: Data will be fetched by accessing the member field obj->FieldName directly
 *  DptrGetter: Data will be fetched by accessing ClassPrivate::get(obj)->FieldName()
 *  DptrMember: Data will be fetched by accessing ClassPrivate::get(obj)->FieldName
 *  CustomCommand: Incompatible with this macro. Use CUSTOM_PROP instead.
 *
 *  NonConst: Indicates that the getter is non-const, thus making the wrapped
 *            getter non-const, too.
 *  QProp: Indicates that there exists a Qt property with this name. Setting
 *         this flag will enable reading, writing as well as automatic updating.
 *  OwningPointer: Indicates that the object owns the object which this property
 *                 points to. Setting this correctly is crucial for memory
 *                 management of the object wrapper.
 *  NonOwningPointer Indicates that this object does not own the object which
 *                   this property points to. Setting this correctly is crucial
 *                   for memory management of the object wrapper.
 *
 * It is necessary to set one of Getter/MemberVar/DptrGetter/DptrMember.
 * Further, for properties that are pointers to other wrappable
 * objects, it's necessary to set either OwningPointer or NonOwningPointer.
 *
 * Example: If you used obj->x() before to access some data, you can make that
 * available to the wrapper, by writing `PROP(x, Getter)`. Later, use wrapper.x()
 * to access it.
 */
#define RO_PROP(FieldName, Flags) \
DEFINE_COUNTER(W_COUNTER_##FieldName, __data) \
DEFINE_FETCH_FUNCTION_PROP(FieldName) \
DATA_APPEND(W_COUNTER_##FieldName, typename std::decay<decltype(wrapPhase1<Flags>(fetch_##FieldName<Flags>(static_cast<value_type*>(nullptr))))>::type, wrapPhase1<Flags>(fetch_##FieldName<Flags>(d->object<value_type>()))) \
DEFINE_GETTER(FieldName, W_COUNTER_##FieldName - 1, Flags) \
DEFINE_REFRESH_PROPERTY(FieldName, W_COUNTER_##FieldName - 1, Flags) \
ADD_TO_METAOBJECT(FieldName, decltype(wrap<Flags>(fetch_##FieldName<Flags>(static_cast<value_type*>(nullptr)))), Flags) \
CONNECT_TO_UPDATES(FieldName, Flags) \


/**
 * Adds a property to the object wrapper. The data will be accessible
 * through a getter in the wrapper, named as \p FieldName. Data will be writable
 * through a setter in the wrapper, named as \p SetterName.
 *
 * The property can be customized by a couple of \p Flags:
 *  Getter: If this flag is set, data will be fetched using obj->FieldName()
 *          and written using obj->SetterName(newVal).
 *  MemberVar: Data will be fetched/written by accessing the member field
 *             obj->FieldName directly.
 *  DptrGetter: Data will be fetched by accessing ClassPrivate::get(obj)->FieldName()
 *              and written using ClassPrivate::get(obj)->SetterName(newVal).
 *  DptrMember: Data will be fetched/written by accessing
 *              ClassPrivate::get(obj)->FieldName.
 *  CustomCommand: Incompatible with this macro. Use CUSTOM_PROP instead.
 *
 *  NonConst: Indicates that the getter is non-const, thus making the wrapped
 *            getter non-const, too.
 *  QProp: Indicates that there exists a Qt property with this name. Setting
 *         this flag will enable reading, writing as well as automatic updating.
 *  OwningPointer: Indicates that the object owns the object which this property
 *                 points to. Setting this correctly is crucial for memory
 *                 management of the object wrapper.
 *  NonOwningPointer Indicates that this object does not own the object which
 *                   this property points to. Setting this correctly is crucial
 *                   for memory management of the object wrapper.
 *
 * It is necessary to set one of Getter/MemberVar/DptrGetter/DptrMember.
 * Further, for properties that are pointers to other wrappable
 * objects, it's necessary to set either OwningPointer or NonOwningPointer.
 *
 * Example: If you used obj->x() before to access some data, you can make that
 * available to the wrapper, by writing `PROP(x, Getter)`. Later, use wrapper.x()
 * to access it.
 */
#define RW_PROP(FieldName, SetterName, Flags) \
DEFINE_COUNTER(W_COUNTER_##FieldName, __data) \
DEFINE_FETCH_FUNCTION_PROP(FieldName) \
DEFINE_WRITE_FUNCTION_PROP(FieldName, SetterName) \
DATA_APPEND(W_COUNTER_##FieldName, typename std::decay<decltype(wrapPhase1<Flags>(fetch_##FieldName<Flags>(static_cast<value_type*>(nullptr))))>::type, wrapPhase1<Flags>(fetch_##FieldName<Flags>(d->object<value_type>()))) \
DEFINE_GETTER(FieldName, W_COUNTER_##FieldName - 1, Flags) \
DEFINE_SETTER(FieldName, SetterName, W_COUNTER_##FieldName - 1, Flags) \
DEFINE_REFRESH_PROPERTY(FieldName, W_COUNTER_##FieldName - 1, Flags) \
ADD_TO_METAOBJECT(FieldName, decltype(wrap<Flags>(fetch_##FieldName<Flags>(static_cast<value_type*>(nullptr)))), Flags) \
CONNECT_TO_UPDATES(FieldName, Flags) \

/**
 * Adds a property to the object wrapper. The data will be accessible
 * through a getter in the wrapper, named as \p FieldName. The value of the
 * property will be given by evaluating the expression \p Expression in a
 * context where `object` is a valid C-pointer pointing to the wrapped object.
 *
 * The property can be customized by a couple of \p Flags:
 *  Getter, MemberVar, DptrGetter, DptrMember: Incompatible
 *      with this macro. Use PROP instead.
 *  CustomCommand: Optional when using this macro. Indicates that data is to be
 *                 fetched using the custom command \p Expression.
 *
 *  NonConst: Indicates that the expression is non-const, thus making the wrapped
 *            getter non-const, too.
 *  QProp: Indicates that there exists a Qt property with this name. Setting
 *         this flag will enable reading, writing as well as automatic updating.
 *  OwningPointer: Indicates that the object owns the object which this property
 *                 points to. Setting this correctly is crucial for memory
 *                 management of the object wrapper.
 *  NonOwningPointer Indicates that this object does not own the object which
 *                   this property points to. Setting this correctly is crucial
 *                   for memory management of the object wrapper.
 *
 * For properties that are pointers to other wrappable objects, it's necessary
 * to set either OwningPointer or NonOwningPointer.
 *
 * Example: Let Utils::getQmlId(QQuickItem*) be defined. To add a property id to
 * the wrapper of QQuickItem, use `CUSTOM_PROP(id, Utils::getQmlId(object), CustomCommand)`.
 * Later, use wrapper.id() to access it.
 */
// FIXME: C++14 remove the argument `Type` again and use return type deduction, instead.
#define CUSTOM_PROP(FieldName, Type, Expression, Flags) \
DEFINE_COUNTER(W_COUNTER_##FieldName, __data) \
DEFINE_FETCH_FUNCTION_CUSTOM_EXPRESSION(FieldName, Type, Expression) \
DATA_APPEND(W_COUNTER_##FieldName, \
typename std::decay<decltype(wrapPhase1<Flags>(fetch_##FieldName<(Flags) | CustomCommand>(static_cast<value_type*>(nullptr))))>::type, wrapPhase1<Flags>(fetch_##FieldName<(Flags) | CustomCommand>(d->object<value_type>()))) \
DEFINE_GETTER(FieldName, W_COUNTER_##FieldName - 1, (Flags) | CustomCommand) \
DEFINE_REFRESH_PROPERTY(FieldName, W_COUNTER_##FieldName - 1, (Flags) | CustomCommand) \
ADD_TO_METAOBJECT(FieldName, decltype(wrap<Flags>(fetch_##FieldName<Flags | CustomCommand>(static_cast<value_type*>(nullptr)))), (Flags) | CustomCommand) \


#define DIRECT_ACCESS_METHOD(MethodName) \
template<typename ...Args> auto MethodName(Args &&...args) -> decltype(object()->MethodName(unwrap(args)...)) \
{ \
    return object()->MethodName(unwrap(args)...); \
} \
template<typename ...Args> auto MethodName(Args &&...args) const -> decltype(object()->MethodName(unwrap(args)...)) \
{ \
    return object()->MethodName(unwrap(args)...); \
} \


#define BLOCKING_ASYNC_METHOD(MethodName) \
template<typename ...Args> auto MethodName(Args &&...args) -> decltype(object()->MethodName(args...)) \
{ \
    return ObjectWrapperPrivate::call(object(), &value_type::MethodName, args...).get(); \
} \

#define ASYNC_VOID_METHOD(MethodName) \
template<typename ...Args> void MethodName(Args &&...args) \
{ \
    ObjectWrapperPrivate::callVoid(object(), &value_type::MethodName, args...); \
} \

/**
 * Put this macro in the va_args area of DECLARE_OBJECT_WRAPPER to disable
 * caching for this class. Disabling caching means that accessing the wrapped
 * getters will always return the live value by accessing the underlying
 * getter/member directly.
 *
 * Disabling caching is mainly meant as a porting aid.
 */
#define DISABLE_CACHING using disableCaching_t = void;


/**
 * Put this macro in the va_args area of DECLARE_OBJECT_WRAPPER to
 * set the name of the private (pimpl-) class of the type, you're
 * wrapping. This is necessary for properties of type DptrGetter and
 * DptrMember.
 */
#define PRIVATE_CLASS(PrivateClassName) using pimpl_t = PrivateClassName;



#define DEFINE_FACTORY(Class) \
static std::vector<std::shared_ptr<ObjectWrapperPrivate>(*)(void*)> GammaRay_ObjectWrapper_ ## Class ## _subclassFactories = {}; \


#define DEFINE_FACTORY_WB(Class, BaseClass) \
static std::vector<std::shared_ptr<ObjectWrapperPrivate>(*)(void*)> GammaRay_ObjectWrapper_ ## Class ## _subclassFactories = {}; \
static auto GammaRay_create_ ## Class ## _from_ ## BaseClass ## _dummy = ObjectWrapper<BaseClass>::s_addSubclassFactory(&ObjectWrapperPrivate::createFromBase<Class, BaseClass>);\

#define DEFINE_FACTORY_WB2(Class, BaseClass1, BaseClass2) \
static std::vector<std::shared_ptr<ObjectWrapperPrivate>(*)(void*)> GammaRay_ObjectWrapper_ ## Class ## _subclassFactories = {}; \
static auto GammaRay_create_ ## Class ## _from_ ## BaseClass1 ## _dummy = ObjectWrapper<BaseClass1>::s_addSubclassFactory(&ObjectWrapperPrivate::createFromBase<Class, BaseClass1>);\
static auto GammaRay_create_ ## Class ## _from_ ## BaseClass2 ## _dummy = ObjectWrapper<BaseClass2>::s_addSubclassFactory(&ObjectWrapperPrivate::createFromBase<Class, BaseClass2>);\


#define OBJECT_WRAPPER_COMMON(Class, ...) \
public: \
    using value_type = Class; \
    using ThisClass_t = ObjectWrapper<Class>; \
 \
    static MetaObject *staticMetaObject() { \
        static auto mo = PropertyCache_t::createStaticMetaObject(QStringLiteral(#Class)); \
        return mo.get(); \
    } \
 \
    MetaObject *metaObject() { \
        return d_ptr()->metaObject(); \
    } \
 \
    explicit ObjectWrapper<Class>() = default; \
 \
    static std::vector<std::shared_ptr<ObjectWrapperPrivate>(*)(void*)> &s_subclassFactories() \
    { return GammaRay_ObjectWrapper_ ## Class ## _subclassFactories; } \
    static int s_addSubclassFactory(std::shared_ptr<ObjectWrapperPrivate>(*factory)(void*)) \
    { GammaRay_ObjectWrapper_ ## Class ## _subclassFactories.push_back(factory); return 0; } \
 \
private: \
    friend class ObjectWrapperTest; \
    friend class ObjectHandle<Class>; \
public: \



/**
 * Defines a specialization of the dummy ObjectWrapper class template for
 * \p Class. This is the main macro for enabling wrapping capabilities for a
 * given class.
 *
 * This macro has two arguments. The first one is the name of the class to be
 * wrapped. The second argument is a free-form area that can be used to put
 * arbitrary content into the wrapper class. Its mostly meant, though, to put
 * PROP and CUSTOM_PROP macros in there, which define properties, the wrapper
 * will have. Also put DISABLE_CACHING here, if desired.
 */
#define DEFINE_OBJECT_WRAPPER(Class) \
namespace GammaRay { \
DEFINE_FACTORY(Class) \
template<> \
class ObjectWrapper<Class> \
{ \
public: \
    static std::tuple<> __data(ObjectWrapperPrivate *, __number<0>) { return {}; } \
    static void __metadata(__number<0>, MetaObject *) {} \
    static void __connectToUpdates(ObjectWrapperPrivate *, __number<0>) {} \
    \
    Class *object() const \
    { \
        return d ? d->object<Class>() : nullptr; \
    } \
 \
    using PropertyCache_t = PropertyCache<Class>; \
 \
    explicit ObjectWrapper<Class>(std::shared_ptr<ObjectWrapperPrivate> controlBlock) \
    : d(std::move(controlBlock)) \
    {} \
 \
    void _clear() { d.reset(); } \
 \
    ObjectWrapperPrivate *d_ptr() const { return d.get(); } \
    std::shared_ptr<ObjectWrapperPrivate> cloneD() const { return d; } \
protected: \
    std::shared_ptr<ObjectWrapperPrivate> d; \
 \
    OBJECT_WRAPPER_COMMON(Class)


/**
 * Defines a specialization of the dummy ObjectWrapper class template for
 * \p Class. This is the main macro for enabling wrapping capabilities for a
 * given class.
 *
 * This macro has three arguments. The first one is the name of the class to be
 * wrapped. The second argument is a (the) base class of \p Class, which already
 * has a wrapper class defined. The third argument is a free-form area that can
 * be used to put arbitrary content into the wrapper class. Its mostly meant,
 * though, to put PROP and CUSTOM_PROP macros in there, which define properties,
 * the wrapper will have. Also put DISABLE_CACHING here, if desired.
 */
#define DEFINE_OBJECT_WRAPPER_WB(Class, BaseClass, ...) \
namespace GammaRay { \
DEFINE_FACTORY_WB(Class, BaseClass) \
template<> \
class ObjectWrapper<Class> : public ObjectWrapper<BaseClass> \
{ \
public: \
    /* We hide the base classes __data and __metadata functions on purpose and start counting at 0 again. */ \
    static std::tuple<> __data(ObjectWrapperPrivate *, __number<0>) { return {}; } \
    static void __metadata(__number<0>, MetaObject *) {} \
    static void __connectToUpdates(ObjectWrapperPrivate * d, __number<0>) { ObjectWrapper<BaseClass>::__connectToUpdates(d, __number<255>{}); } \
    \
    using PropertyCache_t = PropertyCache<Class, BaseClass>; \
    \
    explicit ObjectWrapper<Class>(std::shared_ptr<ObjectWrapperPrivate> controlBlock) \
        : ObjectWrapper<BaseClass>(std::move(controlBlock)) \
    {} \
    \
    Class *object() const \
    { \
        return d ? d->object<Class>() : nullptr; \
    } \
 \
    ObjectWrapperPrivate *d_ptr() const { return d.get(); } \
    std::shared_ptr<ObjectWrapperPrivate> cloneD() const { return d; } \
 \
    OBJECT_WRAPPER_COMMON(Class)

/**
 * Defines a specialization of the dummy ObjectWrapper class template for
 * \p Class. This is the main macro for enabling wrapping capabilities for a
 * given class.
 *
 * This macro has three arguments. The first one is the name of the class to be
 * wrapped. The second argument is a (the) base class of \p Class, which already
 * has a wrapper class defined. The third argument is a free-form area that can
 * be used to put arbitrary content into the wrapper class. Its mostly meant,
 * though, to put PROP and CUSTOM_PROP macros in there, which define properties,
 * the wrapper will have. Also put DISABLE_CACHING here, if desired.
 */
#define DEFINE_OBJECT_WRAPPER_WB2(Class, BaseClass1, BaseClass2, ...) \
namespace GammaRay { \
DEFINE_FACTORY_WB2(Class, BaseClass1, BaseClass2) \
template<> \
class ObjectWrapper<Class> : public ObjectWrapper<BaseClass1>, public ObjectWrapper<BaseClass2> \
{ \
public: \
    /* We hide the base classes __data and __metadata functions on purpose and start counting at 0 again. */ \
    static std::tuple<> __data(ObjectWrapperPrivate *, __number<0>) { return {}; } \
    static void __metadata(__number<0>, MetaObject *) {} \
    static void __connectToUpdates(ObjectWrapperPrivate * d, __number<0>) { \
        ObjectWrapper<BaseClass1>::__connectToUpdates(d, __number<255>{}); \
        ObjectWrapper<BaseClass2>::__connectToUpdates(d, __number<255>{}); \
    } \
    \
    using PropertyCache_t = PropertyCache<Class, BaseClass1, BaseClass2>; \
    \
    explicit ObjectWrapper<Class>(std::shared_ptr<ObjectWrapperPrivate> controlBlock) \
    : ObjectWrapper<BaseClass1>(controlBlock) \
    , ObjectWrapper<BaseClass2>(std::move(controlBlock)) \
    {} \
    \
    Class *object() const \
    { \
        return d_ptr() ? d_ptr()->object<Class>() : nullptr; \
    } \
    \
    void _clear() \
    { \
        ObjectWrapper<BaseClass1>::d.reset(); \
        ObjectWrapper<BaseClass2>::d.reset(); \
    } \
 \
    ObjectWrapperPrivate *d_ptr() const { return ObjectWrapper<BaseClass1>::d.get(); } \
    std::shared_ptr<ObjectWrapperPrivate> cloneD() const { return ObjectWrapper<BaseClass1>::d; } \
    \
    OBJECT_WRAPPER_COMMON(Class)

#define OBJECT_WRAPPER_END(Class) \
}; \
} \
Q_DECLARE_METATYPE(GammaRay::ObjectWrapper<Class>) \
Q_DECLARE_METATYPE(GammaRay::ObjectHandle<Class>) \
Q_DECLARE_METATYPE(GammaRay::ObjectView<Class>)


namespace GammaRay {

template< class... > using void_t = void; // C++17: Use std::void_t.
template< class... > struct TemplateParamList {};

template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) // C++14: Use std::make_unique
{
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template<int N> struct __number : public __number<N - 1> {
    static constexpr int value = N;
    static constexpr __number<N-1> prev() { return {}; }
};
// Specialize for 0 to break the recursion.
template<> struct __number<0> { static constexpr int value = 0; };

template<typename T1, typename ...Rest> struct first
{
    using type = T1;
};
template<typename ...Args> using first_t = typename first<Args...>::type;
template<typename T1, typename T2> using second_t = T2;

template<typename T, typename Enable = void>
struct cachingDisabled : public std::false_type {};
template<typename T>
struct cachingDisabled<T, typename T::disableCaching_t> : public std::true_type {};

template<typename Class, typename ...BaseClasses> class ObjectWrapper {};

template<typename Wrapper, typename Enable = void>
struct isSpecialized : public std::false_type {};
template<typename Wrapper>
struct isSpecialized<Wrapper, void_t<typename Wrapper::ThisClass_t>>
 : public std::true_type {};

template<typename T>
using propertyCache_t = typename ObjectWrapper<T>::PropertyCache_t;

template<typename T, typename Enable = void>
struct pimplClass { using type = void; };
template<typename T>
struct pimplClass<T, void_t<typename T::pimpl_t>> { using type = typename T::pimpl_t; };
template<typename T> using pimplClass_t = typename pimplClass<T>::type;

// Customization point: You may specialize this for specific, non-polymorphic types.
template<typename Derived_t, typename Base_t>
auto downcast(Base_t b)
    -> typename std::enable_if<std::is_polymorphic<typename std::remove_pointer<Base_t>::type>::value, Derived_t>::type
{
    return dynamic_cast<Derived_t>(b);
}
template<typename Derived_t, typename Base_t>
auto downcast(Base_t)
    -> typename std::enable_if<!std::is_polymorphic<typename std::remove_pointer<Base_t>::type>::value, Derived_t>::type
{
    return nullptr;
}

struct PropertyCacheBase
{
    virtual ~PropertyCacheBase() = default;

    virtual PropertyCacheBase *cache(std::type_index type) = 0;

    /**
     * Just returns the object pointer as this propertycache sees it
     * (might NOT be a valid pointer to the most-derived object)
     */
    virtual void *object() const = 0;

    virtual MetaObject *metaObject() const = 0;
};

class ObjectWrapperPrivate;

template<typename Class, typename PrimaryBaseClass, typename ...SecondaryBaseClasses>
std::tuple<
    std::unique_ptr<propertyCache_t<PrimaryBaseClass>>,
    std::unique_ptr<propertyCache_t<SecondaryBaseClasses>>...
> moveAndCreateBaseclassCaches(
    Class *object,
    std::unique_ptr<propertyCache_t<PrimaryBaseClass>> primaryBaseCache
)
{
    return std::tuple<
    std::unique_ptr<propertyCache_t<PrimaryBaseClass>>,
    std::unique_ptr<propertyCache_t<SecondaryBaseClasses>>...
    > {
        std::move(primaryBaseCache),
        make_unique<propertyCache_t<SecondaryBaseClasses>>(object)...
    };
}

// stolen from boost
template<class T, class U> std::unique_ptr<T> dynamic_pointer_cast( std::unique_ptr<U> && r ) noexcept
{
    (void) dynamic_cast< T* >( static_cast< U* >( nullptr ) );

    static_assert( std::has_virtual_destructor<T>::value, "The target of dynamic_pointer_cast must have a virtual destructor." );

    T * p = dynamic_cast<T*>( r.get() );
    if( p ) r.release();
    return std::unique_ptr<T>( p );
}

struct IncompleteConstructionTag_t {};

template<typename Class, typename ...BaseClasses>
struct PropertyCache final : PropertyCacheBase
{
    using ObjectWrapper_t = ObjectWrapper<Class>;
    using Data_t = decltype( ObjectWrapper_t::__data(static_cast<ObjectWrapperPrivate*>(nullptr), __number<255>()) );
    using value_type = Class;

    Data_t dataStorage;
    std::tuple<std::unique_ptr<propertyCache_t<BaseClasses>>...> m_baseCaches;
    Class *m_object;

    ~PropertyCache() override = default;

    explicit PropertyCache(Class *object)
        : m_baseCaches(make_unique<propertyCache_t<BaseClasses>>(object)...)
        , m_object(object)
    {
    }
    explicit PropertyCache(Class *object, IncompleteConstructionTag_t)
    : m_object(object) {}


    template<typename T = Class>
    static std::unique_ptr<PropertyCacheBase> fromBaseclassCache(
        typename std::enable_if<std::is_same<T, Class>::value && sizeof...(BaseClasses) == 0, Class *>::type,
        std::unique_ptr<PropertyCacheBase> baseCache)
    {
        Q_ASSERT(dynamic_cast<PropertyCache *>(baseCache.get()) != nullptr);
        return baseCache;
    }

    template<typename T = Class>
    static std::unique_ptr<PropertyCacheBase> fromBaseclassCache(
        typename std::enable_if<std::is_same<T, Class>::value && sizeof...(BaseClasses) != 0, Class *>::type object,
        std::unique_ptr<PropertyCacheBase> baseCache)
    {
        using BaseCache_t = propertyCache_t<first_t<BaseClasses...>>;
        if (dynamic_cast<PropertyCache *>(baseCache.get()) != nullptr) {
            // baseCache is already a cache object for type Class, so no need
            // to expand the cache, just return it as it is.
            return baseCache;
        }

        auto directBaseCache = BaseCache_t::fromBaseclassCache(object, std::move(baseCache));

        auto cache = make_unique<PropertyCache>(object, IncompleteConstructionTag_t{});
        cache->m_baseCaches = moveAndCreateBaseclassCaches<Class, BaseClasses...>(
            object,
            dynamic_pointer_cast<BaseCache_t>(std::move(directBaseCache))
        );

        return cache;
    }

    PropertyCacheBase *cache(std::type_index type) override
    {
        if (std::type_index { typeid(decltype(*this)) } == type) {
            return this;
        }

        return getCacheFromBases(TemplateParamList<BaseClasses...>{}, type); // C++17: Use fold expression
    }

    template<size_t I>
    typename std::tuple_element<I, decltype(dataStorage)>::type &get()
    {
        return std::get<I>(dataStorage);
    }


    void update(ObjectWrapperPrivate *d)
    {
        dataStorage = ObjectWrapper<Class>::__data(d, __number<255>());

        updateBases(d, TemplateParamList<BaseClasses...>{}); // C++17: Use fold expression
    }

    static std::unique_ptr<MetaObject> createStaticMetaObject(const QString &className) {
        auto mo = new MetaObjectImpl<Class>;
        mo->setClassName(className);
        ObjectWrapper_t::__metadata(__number<255>(), mo);
        appendBaseclassMetaObjects(mo, TemplateParamList<BaseClasses...>{}); // C++17: Use fold expression
        return std::unique_ptr<MetaObject>{mo};
    }


    void *object() const override
    {
        return m_object;
    }

    MetaObject *metaObject() const override
    {
        return ObjectWrapper_t::staticMetaObject();
    }

private:
    template<typename Head, typename ...Rest>
    void updateBases(ObjectWrapperPrivate *d, TemplateParamList<Head, Rest...>)
    {
        constexpr size_t i = std::tuple_size<decltype(m_baseCaches)>::value - sizeof...(Rest) - 1;
        std::get<i>(m_baseCaches)->update(d);

        updateBases(d, TemplateParamList<Rest...>{});
    }
    void updateBases(ObjectWrapperPrivate *, TemplateParamList<>)
    {}

    template<typename Head, typename ...Rest>
    static void appendBaseclassMetaObjects(MetaObjectImpl<Class> *mo, TemplateParamList<Head, Rest...>)
    {
        mo->addBaseClass(ObjectWrapper<Head>::staticMetaObject());

        appendBaseclassMetaObjects(mo, TemplateParamList<Rest...>{});
    }
    static void appendBaseclassMetaObjects(MetaObjectImpl<Class> *, TemplateParamList<>)
    {}


    PropertyCacheBase *getCacheFromBases(TemplateParamList<>, std::type_index) const
    {
        return nullptr;
    }
    template<typename Head, typename ...Rest>
    PropertyCacheBase *getCacheFromBases(TemplateParamList<Head, Rest...>, std::type_index type) const
    {
        constexpr size_t i = std::tuple_size<decltype(m_baseCaches)>::value - sizeof...(Rest) - 1;
        auto ret = std::get<i>(m_baseCaches)->cache(type);

        if (!ret) {
            ret = getCacheFromBases(TemplateParamList<Rest...>{}, type);
        }

        return ret;
    }
};


class ObjectWrapperPrivate : public std::enable_shared_from_this<ObjectWrapperPrivate>
{
public:
    template<typename T>
    propertyCache_t<T> *cache() const
    {
        auto ret = dynamic_cast<propertyCache_t<T>*>(m_cache->cache(typeid(propertyCache_t<T>)));
        Q_ASSERT(ret);
        return ret;
    }

    template<typename T>
    bool isComplete() const
    {
        return m_cache->cache(typeid(propertyCache_t<T>));
    }

    template<typename T>
    void expandCache(T *obj)
    {
        m_cache = propertyCache_t<T>::fromBaseclassCache(obj, std::move(m_cache));
        ObjectWrapper<T>::__connectToUpdates(this, __number<255>{});
    }

    template<typename T>
    T *object() const
    {
        auto c = cache<T>();
        return c ? c->m_object : nullptr;
    }

    MetaObject *metaObject() const
    {
        return m_cache->metaObject();
    }

    explicit ObjectWrapperPrivate(std::unique_ptr<PropertyCacheBase> cache)
     : m_cache(std::move(cache))
    {}

    inline ~ObjectWrapperPrivate();

    template<typename Class>
    static std::shared_ptr<ObjectWrapperPrivate> create(Class *object);

    template<typename Class, typename BaseClass>
    static std::shared_ptr<ObjectWrapperPrivate> createFromBase(void *obj)
    {
        auto p = GammaRay::downcast<Class*>(reinterpret_cast<BaseClass *>(obj));
        if (p) {
            return ObjectWrapperPrivate::create(p);
        }
        return nullptr;
    }

    template<typename Class, int storageIndex, int Flags, typename CommandFunc_t, typename std::enable_if<!(Flags & QProp)>::type* = nullptr>
    void connectToUpdates(CommandFunc_t, const char*) {}

    template<typename Class, int storageIndex, int Flags, typename CommandFunc_t, typename std::enable_if<(Flags & QProp) != 0>::type* = nullptr>
    void connectToUpdates(CommandFunc_t command, const char* propertyName);

    template<typename Class, int storageIndex, typename CommandFunc_t, typename SignalFunc_t>
    void connectToUpdates(CommandFunc_t command, SignalFunc_t signal);

    std::vector<QMetaObject::Connection> connections;
    QSemaphore semaphore { 1 };

    template<typename T, typename Func, typename ...Args>
    static void callVoid(T *object, Func &&f, Args &&...args);

    template<typename T, typename Func, typename ...Args>
    static auto call(T *object, Func &&f, Args &&...args) -> std::future<decltype((std::declval<T*>()->*f)(args...))>;


    template<typename T>
    static typename std::enable_if<std::is_base_of<QObject, T>::value, ObjectId>::type objectId(T *object)
    {
        return ObjectId {object};
    }

    template<typename T>
    static typename std::enable_if<!std::is_base_of<QObject, T>::value, ObjectId>::type objectId(T *object)
    {
        return ObjectId { object, ObjectWrapper<T>::staticMetaObject()->className().toLatin1() }; // FIXME what's the correct encoding here?
    }

private:
    std::unique_ptr<PropertyCacheBase> m_cache;
};


template<typename T> class ObjectView;
template<typename T> class ObjectHandle;
template<typename T> bool operator==(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
template<typename T> bool operator!=(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
template<typename T> bool operator==(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
template<typename T> bool operator!=(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);

template<typename T> bool operator<(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
template<typename T> bool operator>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
template<typename T> bool operator<=(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
template<typename T> bool operator>=(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);

template<typename T> bool operator<(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
template<typename T> bool operator>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
template<typename T> bool operator<=(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
template<typename T> bool operator>=(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);

template<typename T>
class ObjectHandle
{
    public:
    using value_type = T;

    static_assert(isSpecialized<ObjectWrapper<T>>::value, "Can't create ObjectHandle: ObjectWrapper is not specialized on this type. Use DECLARE_OBJECT_WRAPPER to define a sepecialization.");

    explicit ObjectHandle(std::shared_ptr<ObjectWrapperPrivate> controlBlock);
    explicit ObjectHandle(ObjectWrapper<T> wrapper) : m_d(std::move(wrapper)) {}
    /*implicit*/ ObjectHandle(std::nullptr_t) {}
    ObjectHandle() = default;




    explicit operator bool() const;
    explicit operator T*() const;

    template<typename U = T>
    typename std::enable_if<!std::is_base_of<QObject, U>::value, bool>::type isValid() const
    {
        return m_d.object();
    }
    template<typename U = T>
    typename std::enable_if<std::is_base_of<QObject, U>::value, bool>::type isValid() const
    {
        return Probe::instance()->isValidObject(m_d.object());
    }

    friend bool operator==(const ObjectHandle<T> &lhs, const ObjectHandle<T> &rhs)
    {
        return lhs.m_d.d_ptr() == rhs.m_d.d_ptr();
    }
    friend bool operator!=(const ObjectHandle<T> &lhs, const ObjectHandle<T> &rhs)
    {
        return lhs.m_d.d_ptr() != rhs.m_d.d_ptr();
    }

    friend bool operator== <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator!= <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator== <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator!= <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator<  <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator>  <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator<= <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator>= <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator<  <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator>  <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator<= <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator>= <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);

    friend bool operator<(const ObjectHandle<T> &lhs, const ObjectHandle<T> &rhs)
    {
        return lhs.m_d.d_ptr() < rhs.m_d.d_ptr();
    }
    friend bool operator>(const ObjectHandle<T> &lhs, const ObjectHandle<T> &rhs)
    {
        return lhs.m_d.d_ptr() > rhs.m_d.d_ptr();
    }
    friend bool operator<=(const ObjectHandle<T> &lhs, const ObjectHandle<T> &rhs)
    {
        return lhs.m_d.d_ptr() <= rhs.m_d.d_ptr();
    }
    friend bool operator>=(const ObjectHandle<T> &lhs, const ObjectHandle<T> &rhs)
    {
        return lhs.m_d.d_ptr() >= rhs.m_d.d_ptr();
    }

    template<typename U>
    operator ObjectHandle<U>() const
    {
        static_assert(std::is_base_of<U, T>::value, "Cannot cast ObjectHandle to type U: U is not a baseclass of T.");
        static_assert(isSpecialized<ObjectWrapper<T>>::value, "Cannot cast ObjectHandle to baseclass U: ObjectWrapper is not specialized on type U. Use DECLARE_OBJECT_WRAPPER to define a sepecialization.");

        return ObjectHandle <U> { static_cast<ObjectWrapper<U>>(m_d) };
    }

    template<typename U>
    operator ObjectView<U>() const
    {
        static_assert(std::is_base_of<U, T>::value, "Cannot cast ObjectView to type U: U is not a baseclass of T.");
        static_assert(isSpecialized<ObjectWrapper<T>>::value, "Cannot cast ObjectView to baseclass U: ObjectWrapper is not specialized on type U. Use DECLARE_OBJECT_WRAPPER to define a sepecialization.");

        return ObjectView <U> { static_cast<ObjectWrapper<U>>(m_d).cloneD() };
    }

    inline const ObjectWrapper<T> *operator->() const;
    inline ObjectWrapper<T> *operator->();
    inline const ObjectWrapper<T> &operator*() const;
    inline ObjectWrapper<T> &operator*();

    inline T *object() const;
    inline T *data() const;
    inline ObjectId objectId() const;

    inline void clear();


    static MetaObject *staticMetaObject();

    void refresh();

private:
    ObjectWrapper<T> m_d;
};

template<typename T>
uint qHash(const ObjectHandle<T> &key, uint seed)
{
    return qHash(key.object(), seed);
}

template<typename T>
class ObjectView
{
public:
    ObjectView() = default;
    /*implicit*/ ObjectView(std::nullptr_t) {}
    explicit ObjectView(std::weak_ptr<ObjectWrapperPrivate> controlBlock);
    explicit operator bool() const;

    template<typename U = T>
    typename std::enable_if<!std::is_base_of<QObject, U>::value, bool>::type isValid() const
    {
        auto d_locked = d.lock();
        return d_locked && d_locked->template object<U>();
    }
    template<typename U = T>
    typename std::enable_if<std::is_base_of<QObject, U>::value, bool>::type isValid() const
    {
        auto d_locked = d.lock();
        return d_locked && Probe::instance()->isValidObject(d_locked->template object<U>()); // FIXME we should not need to lock this just to do a null check
    }


    template<typename U>
    static auto castHelper(const ObjectView<T> &in) -> typename std::enable_if<std::is_base_of<T, U>::value, ObjectView<U>>::type {
        return ObjectView <U> { static_cast<ObjectWrapper<U>>(in.d.lock()).cloneD() };
    }

    template<typename U>
    static auto castHelper(const ObjectView<T> &in) -> typename std::enable_if<std::is_polymorphic<U>::value && std::is_base_of<U, T>::value, ObjectView<U>>::type {
        if (!dynamic_cast<U*>(in.object())) {
            return {};
        }
        return ObjectView <U> { static_cast<ObjectWrapper<U>>(in.d.lock()).cloneD() };
    }

    template<typename U>
    operator ObjectView<U>() const
    {
        static_assert(std::is_base_of<T, U>::value || std::is_base_of<U, T>::value,
                      "Cannot cast ObjectView from type T to type U: Neither is a baseclass of the other.");
        static_assert(isSpecialized<ObjectWrapper<T>>::value, "Cannot cast ObjectView to baseclass U: ObjectWrapper is not specialized on type U. Use DECLARE_OBJECT_WRAPPER to define a sepecialization.");

        return castHelper<U>(*this);
    }
    template<typename U>
    operator ObjectHandle<U>() const
    {
        static_assert(std::is_base_of<U, T>::value, "Cannot cast ObjectView to type U: U is not a baseclass of T.");
        static_assert(isSpecialized<ObjectWrapper<T>>::value, "Cannot cast ObjectView to baseclass U: ObjectWrapper is not specialized on type U. Use DECLARE_OBJECT_WRAPPER to define a sepecialization.");

        return ObjectHandle <U> { static_cast<ObjectWrapper<U>>(d.lock()) };
    }


    friend bool operator== <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator!= <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator== <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator!= <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator<  <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator>  <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator<= <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator>= <T>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs);
    friend bool operator<  <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator>  <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator<= <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);
    friend bool operator>= <T>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs);


    friend bool operator==(const ObjectView<T> &lhs, const ObjectView<T> &rhs)
    {
        return !(lhs != rhs);
    }
    friend bool operator!=(const ObjectView<T> &lhs, const ObjectView<T> &rhs)
    {
        // FIXME: Is owner_less actually enough for comparison? // No, we want the order after wrapping to be the same as unwrapped
        return lhs.object() != rhs.object();
    }

    friend bool operator<(const ObjectView<T> &lhs, const ObjectView<T> &rhs)
    {
        return lhs.object() < rhs.object();
    }
    friend bool operator>(const ObjectView<T> &lhs, const ObjectView<T> &rhs)
    {
        return lhs.object() > rhs.object();
    }
    friend bool operator<=(const ObjectView<T> &lhs, const ObjectView<T> &rhs)
    {
        return lhs.object() <= rhs.object();
    }
    friend bool operator>=(const ObjectView<T> &lhs, const ObjectView<T> &rhs)
    {
        return lhs.object() >= rhs.object();
    }

    static ObjectView nullhandle();

    inline ObjectHandle<T> lock() const;

    // TODO: Do we actually want implicit locking for ObjectView? (Yes, we do!)
    inline const ObjectHandle<T> operator->() const;
    inline ObjectHandle<T> operator->();
    inline const ObjectWrapper<T> &operator*() const;
    inline ObjectWrapper<T> &operator*();
    inline T *object() const;
    inline T *data() const;
    inline ObjectId objectId() const;
    static MetaObject *staticMetaObject();

    inline void clear();

    inline void refresh();

private:
    std::weak_ptr<ObjectWrapperPrivate> d;
};

template<typename T>
uint qHash(const ObjectView<T> &key, uint seed)
{
    return qHash(key.object(), seed);
}

template<typename T>
bool operator==(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs)
{
//     return lhs.m_d.d_ptr() == rhs.d;
    return !(lhs != rhs);
}
template<typename T>
bool operator!=(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs)
{
//     return lhs.m_d.d_ptr() != rhs.d;
    // FIXME: Is owner_less actually enough for comparison? // No, we want the order after wrapping to be the same as unwrapped
    return lhs.object() != rhs.object();
}
template<typename T>
bool operator==(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs)
{
//     return lhs.d == rhs.m_d.d_ptr();
    return !(lhs != rhs);
}
template<typename T>
bool operator!=(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs)
{
//     return lhs.d != rhs.m_d.d_ptr();
    return rhs != lhs;
}

template<typename T>
bool operator<(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs)
{
    // FIXME: Is owner_less actually enough for comparison? // No, we want the order after wrapping to be the same as unwrapped
    return lhs.object() < rhs.object();
}
template<typename T>
bool operator>(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs)
{
    return lhs.object() > rhs.object();
}
template<typename T>
bool operator<=(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs)
{
    return !(lhs > rhs);
}
template<typename T>
bool operator>=(const ObjectHandle<T> &lhs, const ObjectView<T> &rhs)
{
    return !(lhs < rhs);
}

template<typename T>
bool operator<(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs)
{
    return lhs.object() < rhs.object();
}
template<typename T>
bool operator>(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs)
{
    return lhs.object() > rhs.object();
}
template<typename T>
bool operator<=(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs)
{
    return !(lhs > rhs);
}
template<typename T>
bool operator>=(const ObjectView<T> &lhs, const ObjectHandle<T> &rhs)
{
    return !(lhs < rhs);
}


class ObjectShadowDataRepository
{
public:
    static inline ObjectShadowDataRepository *instance();

    template<typename Class>
    static ObjectHandle<Class> handleForObject(Class *obj);

    template<typename Class>
    static ObjectView<Class> viewForObject(Class *obj);

private:
    explicit ObjectShadowDataRepository() = default;
    friend class Probe;

    QHash<void*, std::weak_ptr<ObjectWrapperPrivate>> m_objectToWrapperPrivateMap;

    friend class ObjectWrapperPrivate;
    friend class ObjectWrapperTest;
};

// === Wrapping ===

template<int flags, typename T>
auto wrap(T &&value) -> typename std::enable_if<!isSpecialized<ObjectWrapper<typename std::decay<T>::type>>::value, T>::type
{
    return std::forward<T>(value);
}
template<int flags, typename T>
auto wrap(T *object) -> second_t<typename ObjectWrapper<T>::value_type, typename std::enable_if<(flags & NonOwningPointer) != 0, ObjectView<T>>::type>
{
    return ObjectShadowDataRepository::viewForObject(object);
}
template<int flags, typename T>
auto wrap(T *object) -> second_t<typename ObjectWrapper<T>::value_type, typename std::enable_if<(flags & OwningPointer) != 0, ObjectHandle<T>>::type>
{
    return ObjectShadowDataRepository::handleForObject(object);
}
template<int flags, typename T>
auto wrap(QList<T*> list) -> second_t<typename ObjectWrapper<T>::value_type, typename std::enable_if<(flags & NonOwningPointer) != 0, QList<ObjectView<T>>>::type>
{
    QList<ObjectView<T>> handleList;
    for (T *t : qAsConst(list)) {
        handleList.push_back(ObjectShadowDataRepository::viewForObject(t));
    }
    //     std::transform(list.cbegin(), list.cend(), handleList.begin(), [](T *t) { return ObjectView<T> { t }; });
    return handleList;
}
template<int flags, typename T>
auto wrap(QList<T*> list) -> second_t<typename ObjectWrapper<T>::value_type, typename std::enable_if<(flags & OwningPointer) != 0, QList<ObjectHandle<T>>>::type>
{
    QList<ObjectHandle<T>> handleList;
    for (T *t : qAsConst(list)) {
        handleList.push_back(ObjectShadowDataRepository::handleForObject(t));
    }
    //     std::transform(list.cbegin(), list.cend(), handleList.begin(), [](T *t) { return ObjectView<T> { t }; });
    return handleList;
}
template<int flags, typename T>
auto wrap(QVector<T*> list) -> second_t<typename ObjectWrapper<T>::value_type, typename std::enable_if<(flags & NonOwningPointer) != 0, QVector<ObjectView<T>>>::type>
{
    QVector<ObjectView<T>> handleList;
    handleList.reserve(list.size());
    for (T *t : qAsConst(list)) {
        handleList.push_back(ObjectShadowDataRepository::viewForObject(t));
    }
    //     std::transform(list.cbegin(), list.cend(), handleList.begin(), [](T *t) { return ObjectView<T> { t }; });
    return handleList;
}
template<int flags, typename T>
auto wrap(QVector<T*> list) -> second_t<typename ObjectWrapper<T>::value_type, typename std::enable_if<(flags & OwningPointer) != 0, QVector<ObjectHandle<T>>>::type>
{
    QVector<ObjectHandle<T>> handleList;
    handleList.reserve(list.size());
    for (T *t : qAsConst(list)) {
        handleList.push_back(ObjectShadowDataRepository::handleForObject(t));
    }
    //     std::transform(list.cbegin(), list.cend(), handleList.begin(), [](T *t) { return ObjectView<T> { t }; });
    return handleList;
}


template<int flags, typename T>
auto wrapPhase1(T &&value) -> typename std::enable_if<(flags & ForeignPointerBit) != 0, T>::type
{
    return std::forward<T>(value);
}
template<int flags, typename T>
auto wrapPhase1(T &&value) -> typename std::enable_if<(flags & ForeignPointerBit) == 0, decltype(wrap<flags>(std::forward<T>(value)))>::type
{
    return wrap<flags>(std::forward<T>(value));
}

template<int flags, typename T>
auto wrapPhase2(T &&value) -> typename std::enable_if<(flags & ForeignPointerBit) == 0, decltype(std::forward<T>(value))>::type
{
    return std::forward<T>(value);
}
template<int flags, typename T>
auto wrapPhase2(T &&value) -> typename std::enable_if<(flags & ForeignPointerBit) != 0, decltype(wrap<flags>(std::forward<T>(value)))>::type
{
    return wrap<flags>(std::forward<T>(value));
}

template<typename T, typename ...Args>
auto unwrap(T && value, Args &&...) -> decltype(std::forward<T>(value)) // Actually this is only supposed to support one argument, the variadic argument list is only to declare this as the fallback option.
{
    return std::forward<T>(value);
}
template<typename T>
T *unwrap(const ObjectView<T> &object)
{
    return object.object();
}
template<typename T>
T *unwrap(const ObjectHandle<T> &object)
{
    return object.object();
}
template<typename T>
T *unwrap(ObjectView<T> &object)
{
    return object.object();
}
template<typename T>
T *unwrap(ObjectHandle<T> &object)
{
    return object.object();
}
template<typename T>
auto unwrap(QList<ObjectView<T>> list) -> QList<T*>
{
    QList<T*> unwrappedList;
    for (const auto &t : qAsConst(list)) {
        unwrappedList.push_back(t.object());
    }
    return unwrappedList;
}
template<typename T>
auto unwrap(QList<ObjectHandle<T>> list) -> QList<T*>
{
    QList<T*> unwrappedList;
    for (const auto &t : qAsConst(list)) {
        unwrappedList.push_back(t.object());
    }
    return unwrappedList;
}
template<typename T>
auto unwrap(QVector<ObjectView<T>> list) -> QVector<T*>
{
    QVector<T*> unwrappedList;
    for (const auto &t : qAsConst(list)) {
        unwrappedList.push_back(t.object());
    }
    return unwrappedList;
}
template<typename T>
auto unwrap(QVector<ObjectHandle<T>> list) -> QVector<T*>
{
    QVector<T*> unwrappedList;
    for (const auto &t : qAsConst(list)) {
        unwrappedList.push_back(t.object());
    }
    return unwrappedList;
}


// === PropertyCache ===

template<typename T>
auto checkCorrectThread(T*) -> typename std::enable_if<!std::is_base_of<QObject, T>::value, bool>::type
{
    return true;
}
template<typename T>
auto checkCorrectThread(T *obj) -> typename std::enable_if<std::is_base_of<QObject, T>::value, bool>::type
{
    return obj->thread() == QThread::currentThread();
}
template<typename T>
auto checkValidObject(T *obj) -> typename std::enable_if<!std::is_base_of<QObject, T>::value, bool>::type
{
    return obj != nullptr;
}
template<typename T>
auto checkValidObject(T *obj) -> typename std::enable_if<std::is_base_of<QObject, T>::value, bool>::type
{
    return Probe::instance()->isValidObject(obj);
}


template<typename Class>
std::shared_ptr<ObjectWrapperPrivate> ObjectWrapperPrivate::create(Class *object)
{
    if (!checkValidObject(object)) {
        return {};
    }
    Q_ASSERT_X(checkCorrectThread(object), "ObjectHandle", "ObjectHandles can only be created from the thread which the wrapped QObject belongs to.");


    // Use RRTI to see if we have a wrapper defined for the dynamic type of the object,
    // if so, create and return that.
    for (auto factory : ObjectWrapper<Class>::s_subclassFactories()) {
        auto p = factory(object);
        if (p) {
            return p;
        }
    }

    // Here, nobody else can have a reference to the cache objects yet, so we don't need to
    // guard the access with a semaphore. Also we're in object's thread, so we don't need to guard
    // against asynchronous deletions of object.

    auto cache = make_unique<propertyCache_t<Class>>(object); // recursively creates all baseclasses' caches
    auto d = std::make_shared<ObjectWrapperPrivate>(std::move(cache));
    ObjectWrapper<Class>::__connectToUpdates(d.get(), __number<255>{});

    ObjectShadowDataRepository::instance()->m_objectToWrapperPrivateMap.insert(object, std::weak_ptr<ObjectWrapperPrivate> { d });

    IF_CONSTEXPR (!cachingDisabled<ObjectWrapper<Class>>::value) {
        d->template cache<Class>()->update(d.get());
    }

    return d;
}

template<typename Class, int storageIndex, int Flags, typename CommandFunc_t, typename std::enable_if<(Flags & QProp) != 0>::type*>
void ObjectWrapperPrivate::connectToUpdates(CommandFunc_t fetchFunction, const char* propertyName)
{
    static_assert(std::is_base_of<QObject, Class>::value, "members with notify signals can only be defined for QObject-derived types.");
    auto object = static_cast<QObject*>(cache<Class>()->m_object);
    auto mo = object->metaObject();
    auto prop = mo->property(mo->indexOfProperty(propertyName));

    if (!prop.hasNotifySignal()) {
        return;
    }

    auto weakSelf = std::weak_ptr<ObjectWrapperPrivate> { shared_from_this() }; // C++17: Use weak_from_this()
    auto f = [weakSelf, fetchFunction]() {
        std::cout << "Updating cache."<< storageIndex <<"\n";
        QMutexLocker locker { Probe::objectLock() };
        auto d = weakSelf.lock();
        d->semaphore.acquire();
        QSemaphoreReleaser releaser { d->semaphore };
        d->cache<Class>()->template get<storageIndex>() = wrapPhase1<Flags>(fetchFunction(d->object<Class>()));
    };

    auto connection = QObjectPrivate::connect(object,
                                              prop.notifySignal().methodIndex(),
                                              new QtPrivate::QFunctorSlotObject<decltype(f), 0, QtPrivate::List<>, void>(std::move(f)),
                                              Qt::DirectConnection
    );

    connections.push_back(connection);
}


template<typename Class, int storageIndex, typename CommandFunc_t, typename SignalFunc_t>
void ObjectWrapperPrivate::connectToUpdates(CommandFunc_t command, SignalFunc_t signal)
{
    static_assert(std::is_base_of<QObject, Class>::value, "members with notify signals can only be defined for QObject-derived types.");
    auto object = static_cast<QObject*>(cache<Class>()->m_object);
    auto weakSelf = std::weak_ptr<ObjectWrapperPrivate> { shared_from_this() };
    auto f = [weakSelf, command]() {
        std::cout << "Updating cache."<< storageIndex <<"\n";
        QMutexLocker locker { Probe::objectLock() };
        auto d = weakSelf.lock();
        d->semaphore.acquire();
        QSemaphoreReleaser releaser { d->semaphore };
        d->cache<Class>()->template get<storageIndex>() = command(d->object<Class>());
    };

    auto connection = QObject::connect(object, signal, f);

    connections.push_back(connection);
}


ObjectWrapperPrivate::~ObjectWrapperPrivate()
{
    for (auto &&c : connections) {
        QObject::disconnect(c);
    }
    ObjectShadowDataRepository::instance()->m_objectToWrapperPrivateMap.remove(m_cache->object());
}

// === ObjectHandle ===
template<typename T>
ObjectHandle<T>::ObjectHandle(std::shared_ptr<ObjectWrapperPrivate> d)
    : m_d { std::move(d) }
{}

template<typename T>
ObjectHandle<T>::operator bool() const
{
    return isValid();
}

template<typename T>
ObjectHandle<T>::operator T*() const
{
    return object();
}

template<typename T>
const ObjectWrapper<T> *ObjectHandle<T>::operator->() const
{
    return &m_d;
}

template<typename T>
ObjectWrapper<T> *ObjectHandle<T>::operator->()
{
    return &m_d;
}

template<typename T>
const ObjectWrapper<T> &ObjectHandle<T>::operator*() const
{
    return m_d;
}

template<typename T>
ObjectWrapper<T> &ObjectHandle<T>::operator*()
{
    return m_d;
}

template<typename T>
T *ObjectHandle<T>::object() const
{
    return m_d.object();
}
template<typename T>
T *ObjectHandle<T>::data() const
{
    return object();
}
template<typename T>
ObjectId ObjectHandle<T>::objectId() const
{
    return ObjectWrapperPrivate::objectId(object());
}

template<typename T>
void ObjectHandle<T>::clear()
{
    m_d._clear();
}




template<typename T, typename Func, typename ...Args>
void ObjectWrapperPrivate::callVoid(T *object, Func &&f, Args &&...args)
{
    if (!Probe::instance()->isValidObject(object)) {
        return;
    }

    if (object->thread() == QThread::currentThread()) {
        (object->*f)(args...);
    } else {
        T *ptr = object;
        QMetaObject::invokeMethod(object, [ptr, f, args...]() {
            (ptr->*f)(args...);
        }, Qt::QueuedConnection);
    }
}

template<typename T, typename Func, typename ...Args>
auto ObjectWrapperPrivate::call(T *object, Func &&f, Args &&...args) ->
    std::future<decltype((std::declval<T*>()->*f)(args...))>
{
    if (!Probe::instance()->isValidObject(object)) {
        return {};
    }

    std::promise<decltype((object->*f)(args...))> p;
    auto future = p.get_future();
    if (object->thread() == QThread::currentThread()) {
        p.set_value((object->*f)(args...));
    } else {
        T *ptr = object;
        QMetaObject::invokeMethod(object, [p, ptr, f, args...]() {
            p.set_value((ptr->*f)(args...));
        }, Qt::QueuedConnection);
    }
    return future;
}

template<typename T>
void ObjectHandle<T>::refresh()
{
    m_d.d_ptr()->template cache<T>()->update(m_d.d_ptr());
}


template<typename T>
MetaObject *ObjectHandle<T>::staticMetaObject()
{
    return decltype(m_d)::staticMetaObject();
}

// === ObjectView ===

template<typename T>
ObjectView<T>::ObjectView(std::weak_ptr<ObjectWrapperPrivate> controlBlock)
: d(std::move(controlBlock))
{}

template<typename T>
ObjectView<T>::operator bool() const
{
    return isValid();
}

template<typename T>
ObjectHandle<T> ObjectView<T>::lock() const
{
    return ObjectHandle<T> { d.lock() };
}
template<typename T>
ObjectView<T> ObjectView<T>::nullhandle()
{
    return ObjectView<T> { std::weak_ptr<ObjectWrapperPrivate>{} };
}

template<typename T>
const ObjectHandle<T> ObjectView<T>::operator->() const
{
    return lock();
}

template<typename T>
ObjectHandle<T> ObjectView<T>::operator->()
{
    return lock();
}

template<typename T>
const ObjectWrapper<T> &ObjectView<T>::operator*() const
{
    return *lock();
}

template<typename T>
ObjectWrapper<T> &ObjectView<T>::operator*()
{
    return *lock();
}

template<typename T>
T *ObjectView<T>::object() const
{
    auto d_ptr = d.lock();
    return d_ptr ? d_ptr->template object<T>() : nullptr;
}
template<typename T>
T *ObjectView<T>::data() const
{
    return object();
}
template<typename T>
ObjectId ObjectView<T>::objectId() const
{
    return ObjectWrapperPrivate::objectId(object());
}

template<typename T>
void ObjectView<T>::clear()
{
    d.reset();
}
template<typename T>
void ObjectView<T>::refresh()
{
    auto d_ptr = d.lock();
    d_ptr->template cache<T>()->update(d_ptr.get());
}

template<typename T>
MetaObject *ObjectView<T>::staticMetaObject()
{
    return ObjectWrapper<T>::staticMetaObject();
}

// === ObjectShadowDataRepository ===

ObjectShadowDataRepository *ObjectShadowDataRepository::instance()
{
//     return Probe::instance()->objectShadowDataRepository();
    static ObjectShadowDataRepository *self = new ObjectShadowDataRepository();
    return self;
}

template<typename Class>
ObjectHandle<Class> ObjectShadowDataRepository::handleForObject(Class *obj)
{
    if (!obj) {
        return ObjectHandle<Class>{};
    }

    auto self = instance();
    std::shared_ptr<ObjectWrapperPrivate> d {};

    if (self->m_objectToWrapperPrivateMap.contains(obj)) {
        d = self->m_objectToWrapperPrivateMap.value(obj).lock();

        if (!d->isComplete<Class>()) {
            // This happens if the handle for obj was first created as a handle to a base class of Class.
            // In this case, the cache is incomplete and we need to expand it.

            d->expandCache<>(obj);

            IF_CONSTEXPR (!cachingDisabled<ObjectWrapper<Class>>::value) {
                d->cache<Class>()->update(d.get());
            }
        }
    } else {
        d = ObjectWrapperPrivate::create(obj);
    }

    return ObjectHandle<Class> { std::move(d) };
}

template<typename Class>
ObjectView<Class> ObjectShadowDataRepository::viewForObject(Class *obj)
{
    if (!obj) {
        return ObjectView<Class> {};
    }

    auto self = instance();

//     Q_ASSERT_X(self->m_objectToWrapperPrivateMap.contains(obj), "viewForObject", "Obtaining a weak handle requires a (strong) handle to already exist.");

    if (!self->m_objectToWrapperPrivateMap.contains(obj)) {
        return nullptr;
    }

    auto controlPtr = self->m_objectToWrapperPrivateMap.value(obj).lock();
    Q_ASSERT(controlPtr->template isComplete<Class>());
    if (!controlPtr->template isComplete<Class>()) {
        return ObjectView<Class> {};
    }

    return ObjectView<Class> { std::move(controlPtr) };
}

}


// TODO: check updating model, Check threading model, lazy-properties, inheritance-trees
// TODO: Look at Event-monitor, look at Bindings und Signal-Slot connections

// TODO: Do we actually need shared_ptr + weak_ptr or is unique_ptr + raw pointer enough?

#endif // GAMMARAY_OBJECTHANDLE_H