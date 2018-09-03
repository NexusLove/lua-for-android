
#include "luadroid.h"
#include "SpinLock.h"
#include "myarray.h"
#include "java_type.h"
#include "utf8.h"
#include "java_member.h"
#include "lua_object.h"
#include "log_wrapper.h"
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <lua.h>

#if LUA_VERSION_NUM < 503
#include "int64_support.h"
#endif
#define TopErrorHandle(fmt, ...)  \
    ({lua_pushfstring(L,fmt,##__VA_ARGS__);\
context->setPendingException(env,lua_tostring(L,-1));\
    lua_pop(L,1);\
    goto __ErrorHandle;})
#define SetErrorJMP()({if(false){\
__ErrorHandle:\
        luaL_error(L,"");\
return 0;\
}})
#define luaL_isstring(L, i) (lua_type(L,i)==LUA_TSTRING)

static int javaType(lua_State *L)noexcept;

static int javaInstanceOf(lua_State *L)noexcept;

static int javaNew(lua_State *L)noexcept;

static int javaNewArray(lua_State *L)noexcept;

static int javaImport(lua_State *L)noexcept;

static int javaCharValue(lua_State *L)noexcept;

static int javaCharString(lua_State *L)noexcept;

static int javaToJavaObject(lua_State *L)noexcept;

static int javaProxy(lua_State *L)noexcept;

static int javaSync(lua_State *L)noexcept;

static int javaThrow(lua_State *L)noexcept;

static int javaTry(lua_State *L)noexcept;

static int javaPut(lua_State *L)noexcept;

static int javaGet(lua_State *L)noexcept;

static int concatString(lua_State *L)noexcept;

static int objectEquals(lua_State *L)noexcept;

static int javaTypeToString(lua_State *L) noexcept;

static int javaObjectToString(lua_State *L)noexcept;

static int getFieldOrMethod(lua_State *L)noexcept;

static int setFieldOrArray(lua_State *L)noexcept;

static int setField(lua_State *L)noexcept;

static int getField(lua_State *L)noexcept;

static int callMethod(lua_State *L)noexcept;

static ScriptContext *getContext(lua_State *L);

static FuncInfo *saveLuaFunction(JNIEnv *env, lua_State *L, ScriptContext *context, int funcIdx);

static int newArray(TJNIEnv *env, lua_State *L, int start, ScriptContext *context, JavaType *type);

static bool parseLuaObject(JNIEnv *env, lua_State *L, ScriptContext *context, int idx,
                           ValidLuaObject &luaObject);

static bool parseCrossThreadLuaObject(JNIEnv *env, lua_State *L, ScriptContext *context, int idx,
                                      CrossThreadLuaObject &luaObject);

static bool pushLuaObject(TJNIEnv *env, lua_State *L, ScriptContext *context,
                          const CrossThreadLuaObject &luaObject);

static inline void pushJavaType(lua_State *L,JavaType* type);

static void
loadLuaFunction(TJNIEnv *env, lua_State *L, const FuncInfo *info, ScriptContext *context);

static void checkLuaType(TJNIEnv *env, lua_State *L, JavaType *expected, ValidLuaObject &luaObject);

extern bool changeClassName(String &className);

static void pushMember(ScriptContext *context, lua_State *L, bool isStatic, int fieldCount, bool isMethod);

static void
pushArrayElement(TJNIEnv *env, lua_State *L, ScriptContext *context, const JavaObject *obj,
                 JavaType *component);

static void
readArguments(TJNIEnv *env, lua_State *L, ScriptContext *context, Vector<JavaType *> &types,
              Vector<ValidLuaObject> &objects, int start);

static void parseTryTable(lua_State *L);

static void recordLuaError(TJNIEnv *env, ScriptContext *context, lua_State *L, int ret);

static int getObjectLength(lua_State *L);

static LocalFunctionInfo *saveLocalFunction(lua_State *L, int i);

static void pushMetaTable(TJNIEnv *env, lua_State *L, ScriptContext *context,
                          LuaTable<CrossThreadLuaObject> *metaTable);

static bool
readProxyMethods(TJNIEnv *env, lua_State *L, ScriptContext *context, Vector<JavaType *> &interfaces,
                 JavaType *main, Vector<std::unique_ptr<BaseFunction>> &luaFuncs,
                 Vector<JObject> &agentMethods);

extern "C" {
jlong compile(TJNIEnv *env, jclass thisClass, jlong ptr, jstring script, jboolean isFile);
jlong nativeOpen(TJNIEnv *env, jobject object, jboolean importAll);
void registerLogger(TJNIEnv *, jclass, jlong ptr, jobject out, jobject err);
void nativeClose(JNIEnv *env, jclass thisClass, jlong ptr);
void nativeClean(JNIEnv *env, jclass thisClass, jlong ptr);
void releaseFunc(JNIEnv *env, jclass thisClass, jlong ptr);
jint getClassType(TJNIEnv * env, jclass, jlong ptr,jclass clz);
void addJavaObject(TJNIEnv *env, jclass thisClass, jlong ptr, jstring _name, jobject obj,
                   jboolean local);
void addJavaMethod(TJNIEnv *env, jclass thisClass, jlong ptr, jstring jname, jstring method,
                   jobject jinst, jboolean local);
jobjectArray runScript(TJNIEnv *env, jclass thisClass, jlong ptr, jobject script,
                       jboolean isFile,
                       jobjectArray args);
jobject constructChild(TJNIEnv *env, jclass thisClass, jlong ptr, jclass target,
                       jlong nativeInfo);
jobject invokeLuaFunction(TJNIEnv *env, jclass thisClass, jlong ptr,
                          jboolean isInterface,
                          jlong funcRef, jobject proxy, jintArray argTypes,
                          jobjectArray args);
}

static const luaL_Reg javaInterfaces[] =
        {{"type",       javaType},
         {"instanceof", javaInstanceOf},
         {"new",        javaNew},
         {"newArray",   javaNewArray},
         {"import",     javaImport},
         {"proxy",      javaProxy},
         {"sync",       javaSync},
         {"object",     javaToJavaObject},
         {"charString",
                        javaCharString},
         {"charValue",  javaCharValue},
         {"throw",      javaThrow},
         {"try",        javaTry},
         {nullptr,      nullptr}};

static const JNINativeMethod nativeMethods[] =
        {{"nativeOpen",        "(ZZ)J",                            (void *) nativeOpen},
         {"nativeClose",       "(J)V",                             (void *) nativeClose},
         {"nativeClean",       "(J)V",                             (void *) nativeClean},
         {"registerLogger",    "(JLjava/io/OutputStream;"
                                       "Ljava/io/OutputStream;)V", (void *) registerLogger},
         {"compile",           "(JLjava/lang/String;Z)J",          (void *) compile},
         {"runScript",         "(JLjava/lang/Object;Z"
                                       "[Ljava/lang/Object;"
                                       ")[Ljava/lang/Object;",     (void *) runScript},
         {"addObject",         "(JLjava/lang/String;"
                                       "Ljava/lang/Object;Z)V",    (void *) addJavaObject},
         {"addMethod",         "(JLjava/lang/String;"
                                       "Ljava/lang/String;"
                                       "Ljava/lang/Object;Z)V",    (void *) addJavaMethod},
         {"constructChild",    "(JLjava/lang/Class;J)"
                                       "Ljava/lang/Object;",       (void *) constructChild},
         {"releaseFunc",       "(J)V",                             (void *) releaseFunc},
         {"getClassType",      "(JLjava/lang/Class;)I",            (void *) getClassType},
         {"invokeLuaFunction", "(JZJLjava/lang/Object;"
                                       "[I[Ljava/lang/Object;)"
                                       "Ljava/lang/Object;", (void *) invokeLuaFunction}};

JavaVM *vm;
jclass stringType;
jclass classType;
jclass throwableType;
jclass contextClass;
jmethodID objectHash;
jmethodID classGetName;
jmethodID objectToString;
jmethodID sLength;
static __thread jmp_buf errorJmp;

#define JAVA_OBJECT "java_object"
#define JAVA_TYPE "java_type"
#define JAVA_CONTEXT "java_context"
class RegisterKey{};
static const RegisterKey* OBJECT_KEY= reinterpret_cast<const RegisterKey *>(javaInterfaces);
static const RegisterKey* TYPE_KEY=OBJECT_KEY+1;
static const RegisterKey* RETHROW_KEY=TYPE_KEY+1;


#if  LUA_VERSION_NUM == 502
static inline int64_t lua_tointegerx(lua_State* L,int index,int* isnum){
    double num=lua_tonumberx(L,index,isnum);
    int64_t ret=(int64_t)num;
    *isnum=*isnum&&ret==num;
    return ret;

}
#endif
#if LUA_VERSION_NUM < 502
static inline int lua_rawgetp(lua_State* L,int index,const void* p){
    lua_pushlightuserdata(L,(void*)p);
    lua_rawget(L,index);
    return 0;
}
static inline void lua_rawsetp(lua_State* L,int index,const void* p){
    lua_pushlightuserdata(L,(void*)p);
    lua_pushvalue(L,-2);
    lua_rawset(L,index);
    lua_pop(L,1);
}
static inline size_t lua_rawlen(lua_State*L,int index){
    return lua_objlen(L,index);
}

#if LUAJIT_VERSION_NUM<20100 //beta3 only

static inline int64_t lua_tointegerx(lua_State* L,int index,int* isnum){
    double num=lua_tonumber(L,index);
    int64_t ret=(int64_t)num;
    *isnum=lua_isnumber(L,index)&&ret==num;
    return ret;
}
#endif

static const char *luaL_tolstring (lua_State *L, int idx, size_t *len) {
    if (luaL_callmeta(L, idx, "__tostring")) {  /* metafield? */
        if (!lua_isstring(L, -1))
            luaL_error(L, "'__tostring' must return a string");
    }
    else {
        switch (lua_type(L, idx)) {
            case LUA_TNUMBER: {
                double floatVal=lua_tonumber(L, idx);
                int64_t integer= int64_t(floatVal);
                if (floatVal==integer)
                    lua_pushfstring(L, "%I",integer);
                else
                    lua_pushfstring(L, "%f", floatVal);
                break;
            }
            case LUA_TSTRING:
                lua_pushvalue(L, idx);
                break;
            case LUA_TBOOLEAN:
                lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
                break;
            case LUA_TNIL:
                lua_pushliteral(L, "nil");
                break;
            default: {
                int tt = luaL_getmetafield(L, idx, "__name");  /* try name */
                const char *kind = (tt == LUA_TSTRING) ? lua_tostring(L, -1) :
                                   luaL_typename(L, idx);
                lua_pushfstring(L, "%s: %p", kind, lua_topointer(L, idx));
                if (tt != LUA_TNIL)
                    lua_remove(L, -2);  /* remove '__name' */
                break;
            }
        }
    }
    return lua_tolstring(L, -1, len);
}
#endif


static bool testType(lua_State *L, int objIndex, const RegisterKey *key) {
    if (!lua_getmetatable(L, objIndex)) return false;
    lua_rawgetp(L,LUA_REGISTRYINDEX, key);
    bool ret = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return ret;
}

static void* testUData(lua_State* L,int ud,const RegisterKey* key){
    void *p = lua_touserdata(L, ud);
    if (p != NULL) {  /* value is a userdata? */
        if (lua_getmetatable(L, ud)) {  /* does it have a metatable? */
            lua_rawgetp(L,LUA_REGISTRYINDEX, key);  /* get correct metatable */
            if (!lua_rawequal(L, -1, -2))  /* not the same? */
                p = NULL;  /* value is a userdata with wrong metatable */
            lua_pop(L, 2);  /* remove both metatables */
            return p;
        }
    }
    return NULL;  /* value is not a userdata with a metatable */
}
static bool isJavaTypeOrObject(lua_State*L,int index){
    return lua_rawlen(L,index)== sizeof(void *);
}

static JavaObject* checkJavaObject(lua_State* L,int idx){
    JavaObject* ret= static_cast<JavaObject *>(testUData(L, idx, OBJECT_KEY));
    if(unlikely(ret== nullptr))
        luaL_error(L,"Expected " JAVA_OBJECT ",but got %s",luaL_tolstring(L,idx,NULL));
    return ret;
}
static JavaType* checkJavaType(lua_State* L,int idx){
    JavaType** ret= static_cast<JavaType **>(testUData(L, idx, TYPE_KEY));
    if(unlikely(ret== nullptr))
        luaL_error(L,"Expected " JAVA_TYPE ",but got %s",luaL_tolstring(L,idx,NULL));
    return *ret;
}
static void setMetaTable(lua_State* L,const RegisterKey* key){
    lua_rawgetp(L,LUA_REGISTRYINDEX,key);
    lua_setmetatable(L,-2);
}

static bool newMetaTable(lua_State*L,const RegisterKey* key,const char* tname){
    lua_rawgetp(L,LUA_REGISTRYINDEX,key);
    if(!lua_isnil(L,-1)) return false;
    lua_pop(L,1);
    lua_createtable(L, 0, 2);  /* create metatable */
    if(tname){
        lua_pushstring(L, tname);
        lua_setfield(L, -2, "__name");  /* metatable.__name = tname */
    }
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, key);
    return true;
}

static int luaPCallHandler(lua_State *L) {
    if (testType(L, -1, RETHROW_KEY)) {
        return 1;
    }
    const char *s = luaL_tolstring(L, -1, nullptr);
    luaL_traceback(L, L, s, 1);
    return 1;
}

static int luaFullGC(lua_State *L) {
    lua_gc(L, LUA_GCCOLLECT, 0);
    return 0;
}

static int luaGetJava(lua_State *L) {
    int size = sizeof(javaInterfaces) / sizeof(luaL_Reg) - 1;
    lua_createtable(L, 0, size);
    lua_getfield(L,LUA_REGISTRYINDEX,JAVA_CONTEXT);
    luaL_setfuncs(L, javaInterfaces, 1);
    return 1;
}

static int luaPanic(lua_State *L) {
    const char *s = lua_tostring(L, -1);
    if (s) {
        luaL_traceback(L, L, s, 1);
        s = lua_tostring(L, -1);
    } else s = "Unexpected lua error";
    lua_getfield(L,LUA_REGISTRYINDEX,JAVA_CONTEXT);
    ScriptContext* context= static_cast<ScriptContext *>(lua_touserdata(L, -1));
    lua_pop(L,1);
    context->setPendingException(AutoJNIEnv(), s);
    _longjmp(errorJmp, -1);
    return 0;
}

void ScriptContext::config(lua_State *L) {
    lua_atpanic(L, luaPanic);

    lua_pushlightuserdata(L, this);//for panic and lib init
    lua_setfield(L, LUA_REGISTRYINDEX, JAVA_CONTEXT);
    int top = lua_gettop(L);

#if LUA_VERSION_NUM >= 502
    luaL_requiref(L, "java", luaGetJava,/*glb*/true);
#else
    lua_pushlightuserdata(L, this);
    luaL_openlib(L,"java",javaInterfaces,1);
    lua_setglobal(L,"java");
#endif
#if LUA_VERSION_NUM < 503
    Integer64::RegisterTo(L);
#endif
    if (importAll) {
        const luaL_Reg *l = javaInterfaces;
        for (; l->name != NULL; l++) {
            lua_pushlightuserdata(L,this);
            lua_pushcclosure(L, l->func,1);
            lua_setglobal(L, l->name);
        }
        lua_pushlightuserdata(L,this);
        lua_pushcclosure(L, javaType,1);
        lua_setglobal(L, "Type");
    }
    {
        *(ScriptContext**)lua_newuserdata(L, sizeof(void*))=this;
        lua_createtable(L,0,3);
        lua_pushstring(L, "Can't change java metatable");
        lua_setfield(L, -2, "__metatable");
        lua_pushcfunction(L, javaPut);
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, javaGet);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L,-2);
        lua_setglobal(L,"cross");
    }

    if (newMetaTable(L,TYPE_KEY, JAVA_TYPE)) {
        int index = lua_gettop(L);
        lua_pushstring(L, "Can't change java metatable");
        lua_setfield(L, index, "__metatable");
        lua_pushcfunction(L, setFieldOrArray);
        lua_setfield(L, index, "__newindex");
        lua_pushcfunction(L, getFieldOrMethod);
        lua_setfield(L, index, "__index");
        lua_pushlightuserdata(L,this);
        lua_pushcclosure(L, javaNew,1);
        lua_setfield(L, index, "__call");
        lua_pushcfunction(L, javaTypeToString);
        lua_setfield(L, index, "__tostring");
    }
    if (newMetaTable(L,OBJECT_KEY, JAVA_OBJECT)) {
        int index = lua_gettop(L);
        lua_pushstring(L, "Can't change java metatable");
        lua_setfield(L, index, "__metatable");
        lua_pushcfunction(L, setFieldOrArray);
        lua_setfield(L, index, "__newindex");
        lua_pushcfunction(L, getFieldOrMethod);
        lua_setfield(L, index, "__index");
        lua_pushcfunction(L, objectEquals);
        lua_setfield(L, index, "__eq");
        lua_pushcfunction(L, concatString);
        lua_setfield(L, index, "__concat");
        lua_pushcfunction(L, javaObjectToString);
        lua_setfield(L, index, "__tostring");
        lua_pushcfunction(L, getObjectLength);
        lua_setfield(L, index, "__len");
        lua_pushcfunction(L, JavaObject::objectGc);
        lua_setfield(L, index, "__gc");
    }
    newMetaTable(L,RETHROW_KEY, nullptr);
    AutoJNIEnv env;
    for (auto &pair:addedMap) {
        pushAddedObject(env, L, pair.first.data(), pair.second.first.data(), pair.second.second);
    }
    pushJavaType(L,ensureType(env,"com.oslorde.luadroid.ClassBuilder"));
    lua_pushvalue(L, -1);
    lua_setglobal(L, "ClassBuilder");
    lua_settop(L, top);
}

static inline void pushJavaType(lua_State *L,JavaType* type){
    *((JavaType **) lua_newuserdata(L, sizeof(JavaType *))) = type;
    setMetaTable(L, TYPE_KEY);
}

static inline ScriptContext *getContext(lua_State *L) {
    ScriptContext *context = (ScriptContext *) lua_touserdata(L, lua_upvalueindex(1));
    return context;
}

static JavaObject *pushJavaObject(TJNIEnv *env, lua_State *L, ScriptContext *context, jobject obj,JavaType* given= nullptr) {
    JavaObject *objectRef = (JavaObject *) lua_newuserdata(L, sizeof(JavaObject));
    objectRef->object = env->NewGlobalRef(obj);
    objectRef->type = given?given:context->ensureType(env, (JClass) env->GetObjectClass(obj));
    setMetaTable(L, OBJECT_KEY);
    return objectRef;
}
static bool parseLuaObject(JNIEnv *env, lua_State *L, ScriptContext *context, int idx,
                    ValidLuaObject &luaObject) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            luaObject.type = T_NIL;
            break;
        case LUA_TBOOLEAN: {
            luaObject.type = T_BOOLEAN;
            luaObject.isTrue = (jboolean) lua_toboolean(L, idx);
            break;
        }
        case LUA_TSTRING: {
            luaObject.type = T_STRING;
            luaObject.string = lua_tostring(L, idx);
            break;
        }
        case LUA_TFUNCTION: {
            luaObject.type = T_FUNCTION;
            if (context->isLocalFunction()) {
                luaObject.func = saveLocalFunction(L, idx);
            } else {
                luaObject.func = saveLuaFunction(env, L, context, idx);
            }
            break;
        }
        case LUA_TNUMBER: {
#if LUA_VERSION_NUM >= 503
            int isInt;
            lua_Integer intValue = lua_tointegerx(L, idx, &isInt);
            if (isInt) {
                luaObject.type = T_INTEGER;
                luaObject.integer = intValue;
            } else {
                luaObject.type = T_FLOAT;
                luaObject.number = lua_tonumber(L, idx);
            }
#else
            lua_Number number=lua_tonumber(L,idx);
            int64_t intVal=(int64_t)number;
            if(number==intVal){
                luaObject.type=T_INTEGER;
                luaObject.integer=intVal;
            } else{
                luaObject.type=T_FLOAT;
                luaObject.number=number;
            }
#endif
            break;
        }
        case LUA_TLIGHTUSERDATA: {
            luaObject.type = T_INTEGER;
            luaObject.userdata = lua_touserdata(L, idx);
            break;
        }
        case LUA_TUSERDATA:
            if (testUData(L, idx, OBJECT_KEY)) {
                luaObject.type = T_OBJECT;
                luaObject.objectRef = (JavaObject *) lua_touserdata(L, idx);
            }
#if LUA_VERSION_NUM < 503
        else if(luaL_testudata(L,idx,Integer64::LIB_NAME)){
            luaObject.type=T_INTEGER;
            luaObject.integer=((Integer64*)lua_touserdata(L,idx))->m_val;
        }
#endif
            break;
        case LUA_TTABLE: {
            luaObject.type = T_TABLE;
            luaObject.lazyTable = new LazyTable(idx, L);
            break;
        }
        default:
            return false;
    }
    return true;
}

static bool
checkLuaTypeNoThrow(TJNIEnv *env, lua_State *L, JavaType *expected, ValidLuaObject &luaObject) {
    if (expected == nullptr) return false;
    if (expected->isInteger()) {
        if (luaObject.type != T_INTEGER){
            if(luaObject.type==T_FLOAT){
                luaObject.type=T_INTEGER;
                luaObject.integer= int64_t(luaObject.number);
            } else if(luaObject.type==T_OBJECT){
                JavaType* type=luaObject.objectRef->type;
                if(!expected->canAcceptBoxedNumber(type)) goto bail;
                luaObject.type=T_INTEGER;
                luaObject.integer=env->CallLongMethod(luaObject.objectRef->object,longValue);
            } else goto bail;
        }
    } else if (expected->isFloat()) {
        if (luaObject.type != T_FLOAT){
            if(luaObject.type==T_INTEGER){
                luaObject.type=T_FLOAT;
                luaObject.number=luaObject.integer;
            } else if(luaObject.type==T_OBJECT){
                JavaType* type=luaObject.objectRef->type;
                if(!expected->canAcceptBoxedNumber(type)) goto bail;
                luaObject.type=T_FLOAT;
                luaObject.number=env->CallDoubleMethod(luaObject.objectRef->object,doubleValue);
            } else goto bail;
        }
    } else if (expected->isChar()) {
        if (luaObject.type == T_STRING){
            if (strlen8to16(luaObject.string) != 1) {
                lua_pushstring(L, "String length for char type must be 1");
                return true;
            } else{
                luaObject.type=T_CHAR;
                strcpy8to16(&luaObject.character,luaObject.string,NULL);
            }
        } else if(luaObject.type==T_INTEGER){
            if(luaObject.integer<0||luaObject.integer>65535){
                lua_pushstring(L, "The integer is too large for char value");
                return true;
            }else luaObject.type=T_CHAR;
        } else if(luaObject.type==T_OBJECT&&luaObject.objectRef->type->isBoxedChar()){
            luaObject.type=T_CHAR;
            luaObject.character=env->CallCharMethod(luaObject.objectRef->object,charValue);
            return false;
        } else goto bail;

    } else if (expected->isBool()) {
        if (luaObject.type != T_BOOLEAN){
            if(luaObject.type==T_OBJECT&&luaObject.objectRef->type->isBoxedChar()){
                luaObject.type=T_BOOLEAN;
                luaObject.isTrue=env->CallBooleanMethod(luaObject.objectRef->object,booleanValue);
                return false;
            } else goto bail;
        }
    } else {
        if (luaObject.type == T_NIL) return false;
        if (luaObject.type == T_OBJECT) {
            if (luaObject.objectRef->type == expected ||
                env->IsInstanceOf(luaObject.objectRef->object, expected->getType()))
                return false;
            else {
                lua_pushfstring(L, "Incompatible object,expected:%s,received:%s",
                                expected->name(env).str(), luaObject.objectRef->type->name(env).str());
                return true;
            }
        }
        if (expected->isStringAssignable() && luaObject.type == T_STRING) return false;
        if (luaObject.type == T_FUNCTION) {
            if (expected->isSingleInterface(env))return false;
            forceRelease(luaObject);
            lua_pushstring(L, "Can't convert function to a class that is not a single interface");
            return true;
        }
        if (luaObject.type == T_TABLE) {
            if (expected->isTableType(env) ||
                (expected->isInterface(env) && luaObject.lazyTable->isInterface()))
                return false;
        }
        if(luaObject.type==T_FLOAT){
            if(expected->isBoxedFloat()){
                return false;
            }
        } else if(luaObject.type==T_INTEGER){
            if(expected->isBoxedInteger()||expected->isBoxedFloat())
                return false;
        }
        if(expected->isBoxedChar()&&luaObject.type == T_STRING)
            return false;
        if(expected->isBoxedBool()&&luaObject.type == T_BOOLEAN)
            return false;
        goto bail;
    }
    return false;
    bail:
    lua_pushfstring(L, "Expected type is %s,but receive %s",
                    JString(env->CallObjectMethod(expected->getType(), classGetName)).str(),
                    luaObject.typeName());
    return true;
}

void checkLuaType(TJNIEnv *env, lua_State *L, JavaType *expected, ValidLuaObject &luaObject) {
    if (likely(expected == nullptr)) return;
    if (checkLuaTypeNoThrow(env, L, expected, luaObject)) {
        forceRelease(luaObject);
        lua_error(L);
    }
}

void readArguments(TJNIEnv *env, lua_State *L, ScriptContext *context, Vector<JavaType *> &types,
                   Vector<ValidLuaObject> &objects, int start) {
    int top = lua_gettop(L);
    auto expectedSize = top - (start - 1);
    types.reserve(expectedSize);
    objects.reserve(expectedSize);
    for (int i = start; i <= top; ++i) {
        JavaType **typeRef = (JavaType **) testUData(L, i, TYPE_KEY);
        bool noType = typeRef == nullptr;
        JavaType *paramType = noType ? nullptr : *typeRef;
        types.push_back(paramType);
        if (!noType) i++;
        ValidLuaObject luaObject;
        if (!parseLuaObject(env, L, context, i, luaObject)) {
            forceRelease(types, objects);
            luaL_error(L, "Arg unexpected for array");
        }
        if (checkLuaTypeNoThrow(env, L, paramType, luaObject)) {
            forceRelease(luaObject, types, objects);
            lua_error(L);
        }
        objects.push_back(std::move(luaObject));
    }
}


void pushArrayElement(TJNIEnv *env, lua_State *L, ScriptContext *context, const JavaObject *obj,
                      JavaType *componentType) {
    int isnum;
    jlong index = lua_tointegerx(L, 2, &isnum);
    if (unlikely(!isnum)) luaL_error(L, "Invalid index for an array");
    if (unlikely(index > INT32_MAX))luaL_error(L, "Index out of range:%d", index);
    //recycle the jclass;
    switch (componentType->getTypeID()){
#define PushChar(c) ({ \
        char s[4]={0};\
        strncpy16to8(s,(const char16_t *) &c, 1);\
        lua_pushstring(L,s);\
        })
#define ArrayGet(typeID,jtype, jname, TYPE)\
            case JavaType::typeID:{\
                j##jtype buf;\
                env->Get##jname##ArrayRegion((j##jtype##Array) obj->object, (jsize) index, 1, &buf);\
                lua_push##TYPE(L,buf);\
            }
#define IntArrayGet(typeID,jtype, jname) ArrayGet(typeID,jtype,jname,integer)
#define FloatArrayGet(typeID,jtype, jname) ArrayGet(typeID,jtype,jname,number)
        IntArrayGet(BYTE,byte, Byte)
        IntArrayGet(SHORT,short, Short)
        IntArrayGet(INT,int, Int)
        IntArrayGet(LONG,long, Long)
        FloatArrayGet(FLOAT,float, Float)
        FloatArrayGet(DOUBLE,double, Double)
        ArrayGet(BOOLEAN,boolean, Boolean, boolean)
        case JavaType::CHAR: {
            jchar buf;
            env->GetCharArrayRegion((jcharArray) obj->object, (jsize) index, 1, &buf);
            PushChar(buf);
        }
        default: {
            auto ele = env->GetObjectArrayElement((jobjectArray) obj->object, (jsize) index);
            if(ele== nullptr)
                lua_pushnil(L);
            else pushJavaObject(env, L, context, ele);
        }
    }
}

static void pushRawMethod(ScriptContext *context,lua_State *L, bool isStatic) {
    MemberFlag *member = (MemberFlag *) lua_newuserdata(L, sizeof(MemberFlag));
    member->isStatic = isStatic;
    member->isNotOnlyMethod = false;
    member->isField = false;
    member->isDuplicatedField = false;
    member->context=context;
    lua_pushvalue(L, -3);//obj
    lua_pushvalue(L, -3);//name
    lua_pushcclosure(L, callMethod, 3);
}

void pushMember(ScriptContext *context, lua_State *L, bool isStatic, int fieldCount, bool isMethod) {
    if (fieldCount == 0 && isMethod) {
        pushRawMethod(context,L, isStatic);
        return;
    }
    lua_newuserdata(L, 0);//value represent the java member
    lua_createtable(L, 0, 3);//metatable;
    int tableIndex = lua_gettop(L);
    int nameIndex = tableIndex - 2;
    int tOIndex = tableIndex - 3;//class or the obj
    MemberFlag *member = (MemberFlag *) lua_newuserdata(L, sizeof(MemberFlag));
    member->isStatic = isStatic;
    member->isNotOnlyMethod = true;
    member->isField = true;
    member->isDuplicatedField = fieldCount > 1;
    int flagIndex = lua_gettop(L);
    if (fieldCount > 0) {
        lua_pushstring(L, "__index");
        lua_pushvalue(L, flagIndex);
        lua_pushvalue(L, tOIndex);
        lua_pushvalue(L, nameIndex);
        lua_pushcclosure(L, getField, 3);
        lua_rawset(L, tableIndex);

        lua_pushstring(L, "__newindex");
        lua_pushvalue(L, flagIndex);
        lua_pushvalue(L, tOIndex);
        lua_pushvalue(L, nameIndex);
        lua_pushcclosure(L, setField, 3);
        lua_rawset(L, tableIndex);
    }

    if (isMethod) {
        lua_pushstring(L, "__call");
        lua_pushvalue(L, flagIndex);
        lua_pushvalue(L, tOIndex);
        lua_pushvalue(L, nameIndex);
        lua_pushcclosure(L, callMethod, 3);
        lua_rawset(L, tableIndex);
    }

    lua_pop(L, 1);//pop flag

    //to improve performance,comment out below code
    /*lua_pushstring(L,"__metatable");
    lua_pushboolean(L,0);
    lua_rawset(L,tableIndex);*/

    lua_setmetatable(L, -2);
}


void pushMetaTable(TJNIEnv *env, lua_State *L, ScriptContext *context,
                   LuaTable<CrossThreadLuaObject> *metaTable) {
    CrossThreadLuaObject key;
    key.table = metaTable;
    key.type = T_TABLE;
    pushLuaObject(env, L, context, key);
    key.type = T_NIL;
    lua_setmetatable(L, -2);
}

bool parseCrossThreadLuaObject(JNIEnv *env, lua_State *L, ScriptContext *context, int idx,
                               CrossThreadLuaObject &luaObject) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            luaObject.type = T_NIL;
            break;
        case LUA_TBOOLEAN: {
            luaObject.type = T_BOOLEAN;
            luaObject.isTrue = (jboolean) lua_toboolean(L, idx);
            break;
        }
        case LUA_TSTRING: {
            luaObject.type = T_STRING;
            size_t len;
            auto src = lua_tolstring(L, idx, &len);
            char *dest = new char[len];
            strcpy(dest, src);
            luaObject.string = dest;
            break;
        }
        case LUA_TFUNCTION: {
            luaObject.type = T_FUNCTION;
            luaObject.func = saveLuaFunction(env, L, context, idx);
            break;
        }
        case LUA_TNUMBER: {
#if LUA_VERSION_NUM >= 503
            int isInt;
            lua_Integer intValue = lua_tointegerx(L, idx, &isInt);
            if (isInt) {
                luaObject.type = T_INTEGER;
                luaObject.integer = intValue;
            } else {
                luaObject.type = T_FLOAT;
                luaObject.number = lua_tonumber(L, idx);
            }
#else
            lua_Number number=lua_tonumber(L,idx);
            int64_t intVal=(int64_t)number;
            if(number==intVal){
                luaObject.type=T_INTEGER;
                luaObject.integer=intVal;
            } else{
                luaObject.type=T_FLOAT;
                luaObject.number=number;
            }
#endif
            break;
        }
        case LUA_TLIGHTUSERDATA: {
            luaObject.type = T_LIGHT_USER_DATA;
            luaObject.lightData = lua_touserdata(L, idx);
            break;
        }
        case LUA_TUSERDATA:
            if (testUData(L, idx, OBJECT_KEY)) {
                luaObject.type = T_OBJECT;
                JavaObject *orig = (JavaObject *) lua_touserdata(L, idx);
                luaObject.objectRef = new JavaObject;
                luaObject.objectRef->type = orig->type;
                luaObject.objectRef->object = env->NewGlobalRef(orig->object);
            }else if(testUData(L,idx,TYPE_KEY)){
                luaObject.type = T_JAVA_TYPE;
                luaObject.javaType=*(JavaType**) lua_touserdata(L,idx);
            }
#if LUA_VERSION_NUM < 503
                else if(luaL_testudata(L,idx,Integer64::LIB_NAME)){
                luaObject.type=T_INTEGER;
                luaObject.integer=((Integer64*)lua_touserdata(L,idx))->m_val;
            }
#endif

            else {
                lua_pushvalue(L, idx);
                lua_rawget(L, LUA_REGISTRYINDEX);
                UserData *data = static_cast<UserData *>(lua_touserdata(L, -1));
                lua_pop(L,1);
                if (data == nullptr) {
                    size_t len = lua_rawlen(L, idx);
                    data = new UserData(len);
                    lua_pushvalue(L,idx);
                    lua_pushlightuserdata(L,data);
                    lua_rawset(L,LUA_REGISTRYINDEX);
                    int hasMeta = lua_getmetatable(L, idx);
                    if (hasMeta && lua_istable(L, -1)) {
                        CrossThreadLuaObject object;
                        lua_getfield(L,-1,"__gc");
                        bool ok=!lua_isnil(L,-1);
                        lua_pop(L,1);
                        ok=ok&&parseCrossThreadLuaObject(env, L, context, -1, object);
                        lua_pop(L, 1);
                        if (ok) {
                            data->metaTable = object.table;
                            object.type = T_NIL;
                            object.table = nullptr;
                        }
                        if (!ok) {
                            delete data;
                            lua_pushvalue(L,idx);
                            lua_pushnil(L);
                            lua_rawset(L,LUA_REGISTRYINDEX);
                            return false;
                        }
                    }
                    void *orig = lua_touserdata(L, idx);
                    memcpy(&data->data[0], orig, len);
                    luaObject.userData = data;
                    lua_pushvalue(L,idx);
                    lua_pushnil(L);
                    lua_rawset(L,LUA_REGISTRYINDEX);
                } else {
                    luaObject.userData = data;
                }
                luaObject.type = T_USER_DATA;
            }
            break;
        case LUA_TTABLE: {
            lua_pushvalue(L, idx);
            lua_rawget(L, LUA_REGISTRYINDEX);
            LuaTable<CrossThreadLuaObject> *luaTable = (LuaTable<CrossThreadLuaObject> *)(lua_touserdata(L, -1));
            lua_pop(L,1);
            if (luaTable == nullptr) {
                luaTable = new LuaTable<CrossThreadLuaObject>();
                lua_pushvalue(L,idx);
                lua_pushlightuserdata(L,luaTable);
                lua_rawset(L,LUA_REGISTRYINDEX);
                luaTable->get().reserve(int(lua_rawlen(L, idx)));
                lua_pushnil(L);
                while (lua_next(L, idx)) {
                    CrossThreadLuaObject key;
                    CrossThreadLuaObject value;
                    bool ok = parseCrossThreadLuaObject(env, L, context, -2, key) &&
                              parseCrossThreadLuaObject(env, L, context, -1, value);
                    lua_pop(L, 1);
                    if (ok)
                        luaTable->get().push_back({std::move(key), std::move(value)});
                    else {
                        delete luaTable;
                        lua_pushvalue(L,idx);
                        lua_pushnil(L);
                        lua_rawset(L,LUA_REGISTRYINDEX);
                        return false;
                    }
                }

                int hasMeta = lua_getmetatable(L, idx);
                if (hasMeta && lua_istable(L, -1)) {
                    CrossThreadLuaObject object;
                    lua_getfield(L,-1,"__gc");
                    bool ok=!lua_isnil(L,-1);
                    lua_pop(L,1);
                    ok=ok&&parseCrossThreadLuaObject(env, L, context, -1, object);
                    lua_pop(L, 1);
                    if (ok) {
                        luaTable->metaTable = object.table;
                        object.type = T_NIL;
                        object.table = nullptr;
                    } else {
                        delete luaTable;
                        lua_pushvalue(L,idx);
                        lua_pushnil(L);
                        lua_rawset(L,LUA_REGISTRYINDEX);
                        return false;
                    }
                }
                luaObject.table = luaTable;
                lua_pushvalue(L,idx);
                lua_pushnil(L);
                lua_rawset(L,LUA_REGISTRYINDEX);
            } else {
                luaObject.table = luaTable;
            }
            luaObject.type = T_TABLE;
            break;
        }
        default:
            return false;
    }
    return true;
}

bool pushLuaObject(TJNIEnv *env, lua_State *L, ScriptContext *context,
                   const CrossThreadLuaObject &luaObject) {
    switch (luaObject.type) {
        case T_NIL:
            return false;
        case T_BOOLEAN:
            lua_pushboolean(L, luaObject.isTrue);
            break;
        case T_FLOAT:
            lua_pushnumber(L, luaObject.number);
            break;
        case T_INTEGER:
#if LUA_VERSION_NUM >= 503
            lua_pushinteger(L, luaObject.integer);
#else
        if(int64_t(double(luaObject.integer))!=luaObject.integer){
            Integer64::pushLong(L,luaObject.integer);
        } else lua_pushnumber(L,luaObject.integer);
#endif

            break;
        case T_STRING:
            lua_pushstring(L, luaObject.string);
            break;
        case T_OBJECT:
            pushJavaObject(env, L, context, luaObject.objectRef->object);
            break;
        case T_FUNCTION:
            loadLuaFunction(env, L, luaObject.func, context);
            break;
        case T_TABLE:
            lua_rawgetp(L, LUA_REGISTRYINDEX,luaObject.table);
            if (lua_isnil(L, -1)) {
                lua_pop(L, -1);
                lua_createtable(L, 0, luaObject.table->get().size());
                lua_pushlightuserdata(L, luaObject.table);
                lua_pushvalue(L, -2);
                lua_rawset(L, LUA_REGISTRYINDEX);
                for (const auto &pair:luaObject.table->get()) {
                    const CrossThreadLuaObject &key = pair.first;
                    pushLuaObject(env, L, context, key);
                    const CrossThreadLuaObject &value = pair.second;
                    pushLuaObject(env, L, context, value);
                    lua_rawset(L, -3);
                }
                if (luaObject.table->metaTable != nullptr) {
                    pushMetaTable(env, L, context, luaObject.table->metaTable);
                }
                lua_pushlightuserdata(L, luaObject.table);
                lua_pushnil(L);
                lua_rawset(L, LUA_REGISTRYINDEX);
            }
            break;
        case T_LIGHT_USER_DATA:
            lua_pushlightuserdata(L, luaObject.lightData);
            break;
        case T_USER_DATA: {
            lua_rawgetp(L, LUA_REGISTRYINDEX,luaObject.userData);
            if (lua_isnil(L, -1)) {
                lua_pop(L, -1);
                size_t len = luaObject.userData->data.size();
                void *orig = lua_newuserdata(L, len);;
                memcpy(orig, &luaObject.userData->data[0], len);
                if (luaObject.userData->metaTable != nullptr) {
                    lua_pushlightuserdata(L, luaObject.userData);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, LUA_REGISTRYINDEX);
                    pushMetaTable(env, L, context, luaObject.userData->metaTable);
                    lua_pushlightuserdata(L, luaObject.userData);
                    lua_pushnil(L);
                    lua_rawset(L, LUA_REGISTRYINDEX);
                }
            }
            break;
        }
        case T_JAVA_TYPE:
            pushJavaType(L,luaObject.javaType);
            break;
        default:
            return false;
    }
    return true;
}

void recordLuaError(TJNIEnv *env, ScriptContext *context, lua_State *L, int ret) {
    switch (ret) {
        case LUA_ERRERR:
        case LUA_ERRRUN:
#if LUA_VERSION_NUM > 501
        case LUA_ERRGCMM:
#endif
        case LUA_ERRSYNTAX:
            context->setPendingException(env, luaL_tolstring(L, -1, nullptr));
            lua_pop(L, 1);
            break;
        case LUA_ERRMEM:
            context->setPendingException(env, "Lua memory error");
            break;
        case -1:
            context->setPendingException(env, "Unknown error");
        default:
            break;
    }
}

int javaType(lua_State *L) {
    ScriptContext *context = getContext(L);
    AutoJNIEnv env;
    int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        JavaType *type;
        if (luaL_isstring(L, i)) {
            type = context->ensureType(env, lua_tostring(L, i));
            if(type== nullptr)
                goto Error;
        } else {
            JavaObject *objectRef = (JavaObject *) testUData(L, i, OBJECT_KEY);
            if (objectRef != nullptr) {
                if (env->IsInstanceOf(objectRef->object, classType)) {
                    type = context->ensureType(env, (jclass) objectRef->object);
                    goto Ok;
                } else if (env->IsInstanceOf(objectRef->object, stringType)) {
                    type = context->ensureType(env, JString(env, (jstring) objectRef->object));
                    if(type!= nullptr)
                        goto Ok;
                }
            }
            goto Error;
        }
        Ok:
        pushJavaType(L,type);
        continue;//never reach
        Error:
        luaL_error(L, "Invalid type=%s", luaL_tolstring(L, i,NULL));
    }
    return count;
}

static int proxyByTable(lua_State *L) {
    if (!lua_istable(L, 1)) return 0;
    ScriptContext *context = getContext(L);
    SetErrorJMP();
    Vector<JavaType *> interfaces;
    lua_getfield(L, 1, "super");
    JavaType **typeRef;
    JavaType *main = nullptr;
    JavaObject* superObject= nullptr;
    if ((typeRef = (JavaType **) testUData(L, -1, TYPE_KEY)) != nullptr)
        main = *typeRef;
    else if((superObject=(JavaObject*)testUData(L,-1,OBJECT_KEY))!= nullptr){
        main=superObject->type;
    }
    lua_getfield(L, 1, "interfaces");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if ((typeRef = (JavaType **) testUData(L, -1, TYPE_KEY)) != nullptr)
                interfaces.push_back(*typeRef);
            lua_pop(L, 1);
        }
    }
    if (main == nullptr) {
        if (interfaces.size() == 0) {
            context->setPendingException(AutoJNIEnv(), "No class or interfaces");
            goto __ErrorHandle;
        } else {
            main = *interfaces.begin();
            interfaces.erase(interfaces.begin());
        }
    }
    AutoJNIEnv env;
    Vector<std::unique_ptr<BaseFunction>> luaFuncs;
    Vector<JObject> agentMethods;
    lua_getfield(L, 1, "methods");
    if (lua_istable(L, -1)) {
        if (!readProxyMethods(env, L, context, interfaces, main, luaFuncs,
                              agentMethods)) {
            goto __ErrorHandle;
        }
    } else if (lua_isfunction(L, -1)) {
        JObject single(main->getSingleInterface(env));
        if (unlikely(single == nullptr)) TopErrorHandle("All methods should be specified");
        agentMethods.push_back(std::move(single));
        if (context->isLocalFunction()) {
            luaFuncs.emplace_back(saveLocalFunction(L, -1));
        } else {
            luaFuncs.emplace_back(saveLuaFunction(env, L, context, -1));
        }

    }
    lua_getfield(L, 1, "shared");
    bool shared;
    if (lua_isnil(L, -1)) shared = false;
    else if (lua_isboolean(L, -1)) shared = lua_toboolean(L, -1) != 0;
    else if (lua_isnumber(L, -1)) shared = lua_tointeger(L, -1) != 0;
    else shared = true;
    jobject proxy;
    if(superObject){
       proxy = context->proxy(env, main, &interfaces, agentMethods, luaFuncs, shared,
                                     0,superObject->object);
    } else{
        Vector<ValidLuaObject> constructArgs;
        Vector<JavaType *> argTypes;
        lua_getfield(L, 1, "args");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            JavaType *type = nullptr;
            while (lua_next(L, -2)) {
                typeRef = (JavaType **) testUData(L, -1, TYPE_KEY);
                if (typeRef != nullptr) {
                    type = *typeRef;
                } else {
                    ValidLuaObject luaObject;
                    if (!parseLuaObject(env, L, context, -1, luaObject)) {
                        context->setPendingException(env, "Arg type not support");
                        goto __ErrorHandle;
                    }
                    argTypes.push_back(type);
                    constructArgs.push_back(std::move(luaObject));
                    type= nullptr;
                }

            }
        }
        void *constructInfo[] = {&constructArgs, &argTypes};
        proxy = context->proxy(env, main, &interfaces, agentMethods, luaFuncs, shared,
                               (long) &constructInfo);
    }
    if (proxy == INVALID_OBJECT) {
        goto __ErrorHandle;
    }
    for (auto &ptr:luaFuncs) {
        ptr.release();
    }
    pushJavaObject(env, L, context, JObject(env, proxy));
    return 1;
}

bool
readProxyMethods(TJNIEnv *env, lua_State *L, ScriptContext *context, Vector<JavaType *> &interfaces,
                 JavaType *main, Vector<std::unique_ptr<BaseFunction>> &luaFuncs,
                 Vector<JObject> &agentMethods) {
    int expectedLen = int(lua_rawlen(L, -1));
    luaFuncs.reserve(expectedLen);
    agentMethods.reserve(expectedLen);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (!lua_isstring(L, -2)) {
            TopErrorHandle("Not a string for method name=%s", luaL_tolstring(L, -2, NULL));
        }
        FakeString methodName(lua_tostring(L, -2));
        if (strcmp(methodName.data(), "<init>") == 0) {

            TopErrorHandle("Constructor can't be override");
        }
        if (lua_istable(L, -1)) {
            Vector<JavaType *> paramTypes;
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                JavaType **typeRef;
                if ((typeRef = (JavaType **) testUData(L, -1, TYPE_KEY)) != nullptr)
                    paramTypes.push_back(*typeRef);
                else if (lua_isfunction(L, -1)) {
                    JavaType *matchType = main;
                    auto info = main->findMethod(env,methodName, false, paramTypes, nullptr);
                    if (info == nullptr) {
                        for (auto interface:interfaces) {
                            info = interface->findMethod(env,methodName, false, paramTypes, nullptr);
                            if (info != nullptr) {
                                matchType = interface;
                                break;
                            }
                        }
                    }
                    if (info == nullptr) {
                        TopErrorHandle("Can't find matched method "
                                               "for the method:%s", methodName.c_str());
                    }
                    agentMethods.push_back(env->ToReflectedMethod(
                            matchType->getType(), info->id, JNI_FALSE));
                    BaseFunction *func;
                    if (context->isLocalFunction())
                        func = saveLocalFunction(L, -1);
                    else
                        func = saveLuaFunction(env, L, context, -1);
                    func->javaRefCount++;
                    luaFuncs.emplace_back(func);
                    paramTypes.clear();
                }
                lua_pop(L, 1);
            }
        } else if (lua_isfunction(L, -1)) {//proxy for all methods
            BaseFunction *func;
            if (context->isLocalFunction()) {
                func = saveLocalFunction(L, -1);
            } else {
                func = saveLuaFunction(env, L, context, -1);
            }
            auto all = main->findAllObjectMethod(env,methodName);
            bool found = false;
            if (all) {
                for (auto &&info:*all) {
                    func->javaRefCount++;
                    agentMethods.push_back(env->ToReflectedMethod(
                            main->getType(), info.id, JNI_FALSE));
                    luaFuncs.emplace_back(func);
                    found = true;
                }
            }
            for (auto interface:interfaces) {
                all = main->findAllObjectMethod(env,methodName);
                if (!all) continue;
                for (auto &&info:*all) {
                    func->javaRefCount++;
                    agentMethods.push_back(env->ToReflectedMethod(
                            interface->getType(), info.id, JNI_FALSE));
                    luaFuncs.emplace_back(func);
                    found = true;
                }
            }
            if (!found) {
                TopErrorHandle("No method named %s", methodName);
            }
        }
        lua_pop(L, 1);
    }
    return true;
    __ErrorHandle:
    return false;
};

int javaProxy(lua_State *L) {
    if (lua_gettop(L) == 1)
        return proxyByTable(L);
    ScriptContext *context = getContext(L);
    JavaType **typeRef;
    JavaType *main;
    JavaObject* superObject;
    if((superObject=(JavaObject*)testUData(L,1,OBJECT_KEY))!= nullptr){
        main=superObject->type;
    } else{
        main = checkJavaType(L,1);
    }
    SetErrorJMP();

    int top = lua_gettop(L);
    Vector<JavaType *> interfaces;
    int i;
    for (i = 2; i < top; ++i) {
        if ((typeRef = (JavaType **) testUData(L, i, TYPE_KEY)) != nullptr)
            interfaces.push_back(*typeRef);
        else break;
    }
    AutoJNIEnv env;
    Vector<JavaType *> curMethodTypes;
    const char *curMethod = nullptr;
    Vector<JObject> agentMethods;
    Vector<std::unique_ptr<BaseFunction>> luaFuncs;
    bool isLocalFunction = context->isLocalFunction();
    for (int j = i; j <= top; ++j) {
        if (luaL_isstring(L, j)) {
            curMethod = lua_tostring(L, j);
            if (strcmp(curMethod, "<init>") == 0) {
                TopErrorHandle("Constructor can't be override");
            }
        } else if ((typeRef = (JavaType **) testUData(L, j, TYPE_KEY)) != nullptr) {
            curMethodTypes.push_back(*typeRef);
        } else if (lua_isfunction(L, j)) {
            if (curMethod != nullptr) {
                JavaType *matchType = main;
                auto info = main->findMethod(env,FakeString(curMethod), false, curMethodTypes, nullptr);
                if (info == nullptr) {
                    for (auto interface:interfaces) {
                        info = interface->findMethod(env,FakeString(curMethod), false, curMethodTypes,
                                                     nullptr);
                        if (info != nullptr) {
                            matchType = interface;
                            break;
                        }
                    }
                }
                if (unlikely(info == nullptr)) {
                    TopErrorHandle("Can't find matched method "
                                           "for the No.%d method:%s", agentMethods.size(),
                                   curMethod);
                } else
                    agentMethods.push_back(
                            env->ToReflectedMethod(matchType->getType(), info->id, JNI_FALSE));
            } else if (likely(curMethodTypes.size() == 0)) {
                JObject single(main->getSingleInterface(env));
                if (unlikely(single == nullptr)) TopErrorHandle("methods should be specified");
                agentMethods.push_back(std::move(single));
            } else {
                TopErrorHandle("Lambda class should not specify types");
            }
            BaseFunction *function;
            if (isLocalFunction) {
                function = saveLocalFunction(L, j);
            } else {
                function = saveLuaFunction(env, L, context, j);
            }
            function->javaRefCount++;
            luaFuncs.emplace_back(function);
            curMethodTypes.clear();
            i = j + 1;//add stack index on success;
        } else
            break;
    }
    if (luaFuncs.size() == 0) {
        TopErrorHandle("No proxy method");
    }

    if (context->hasErrorPending())
        goto __ErrorHandle;
    bool shared = false;
    if (i <= top)
        if (lua_isboolean(L, i)) {
            shared = lua_toboolean(L, i) == 1;
            ++i;
        }
    jobject proxy;
    if(superObject){
        proxy = context->proxy(env, main, &interfaces, agentMethods, luaFuncs, shared,
                               0,superObject->object);
    } else{
        Vector<ValidLuaObject> constructArgs;
        Vector<JavaType *> argTypes;
        for (; i <= top; ++i) {
            JavaType *type = nullptr;
            typeRef = (JavaType **) testUData(L, i, TYPE_KEY);
            if (typeRef != nullptr) {
                type = *typeRef;
                if (++i > top) {
                    TopErrorHandle("no arg found for the No.%d type", argTypes.size());
                }
            }
            argTypes.push_back(type);
            ValidLuaObject luaObject;
            if (!parseLuaObject(env, L, context, i, luaObject)) {
                context->setPendingException(env, "Arg type not support");
                goto __ErrorHandle;
            }
            constructArgs.push_back(std::move(luaObject));
        }
        void *constructInfo[] = {&constructArgs, &argTypes};
        proxy = context->proxy(env, main, &interfaces, agentMethods, luaFuncs, shared,
                               (long) &constructInfo);
    }
    if (proxy == INVALID_OBJECT) {
        goto __ErrorHandle;
    }
    for (auto &ptr:luaFuncs) {
        ptr.release();
    }
    pushJavaObject(env, L, context, JObject(env, proxy));
    return 1;

}
jobject constructChild(TJNIEnv *env, jclass, jlong ptr, jclass target,
                       jlong nativeInfo) {
    ScriptContext *context = (ScriptContext *) ptr;
    JavaType *type = context->ensureType(env, target);
    void **constructInfo = (void **) nativeInfo;
    Vector<ValidLuaObject> *constructArgs = (Vector<ValidLuaObject> *) constructInfo[0];
    Vector<JavaType *> *argTypes = (Vector<JavaType *> *) constructInfo[1];
    jobject ret = type->newObject(env,*argTypes, *constructArgs);
    if (context->hasErrorPending())
        context->throwToJava();
    return ret;
}

LocalFunctionInfo *saveLocalFunction(lua_State *L, int i) {
    LocalFunctionInfo *info = new LocalFunctionInfo(L);
    lua_pushlightuserdata(L, info);
    lua_pushvalue(L, i);
    lua_settable(L, LUA_REGISTRYINDEX);
    return info;
}

int javaImport(lua_State *L) {
    ScriptContext *context = getContext(L);
    SetErrorJMP();
    AutoJNIEnv env;
    if (!luaL_isstring(L, -1)) luaL_error(L, "Should pass a string for import");
    String s(lua_tostring(L, -1));
    Import *import = context->getImport();
    if(s[0]=='[') TopErrorHandle("Don't import array type!");
    if (strcmp(&s[s.length() - 2], ".*") == 0) {
        s = s.substr(0, s.length() - 1);
        const auto &end = import->packages.end();
        auto iter = std::find(import->packages.begin(), end, s);
        if (iter != end) import->packages.push_back(std::move(s));
    } else {
        size_t start = s.rfind('.');
        String name;
        if (start != String::npos) {
            name = s.substr(start + 1);
        } else name = s;
        if (!changeClassName(s)) {
            TopErrorHandle("Invalid type name:%s", s.c_str());
        }
        JClass c(env->FindClass(&s[0]));
        HOLD_JAVA_EXCEPTION(context, {  TopErrorHandle(" Type:%s not found", s.c_str()); });
        const auto &iter = import->stubbed.find(name);
        if (iter != import->stubbed.end() && !env->IsSameObject(c, (*iter).second->getType())) {
            JString prevName(
                    (JString) env->CallObjectMethod((*iter).second->getType(), classGetName));
            if (strncmp(prevName, "java.lang.", 10) != 0) {
                TopErrorHandle("Only single import is allowed for name: %s"
                                       ",with previous class: %s", name.c_str(), prevName.str());
            }
        }
        auto type = context->ensureType(env, c);
        import->stubbed.emplace(name, type);
        pushJavaType(L,type);
        lua_pushvalue(L, -1);
        lua_setglobal(L, name.data());
        return 1;
    }
    return 0;
}

int javaCharValue(lua_State *L) {
    if (!luaL_isstring(L, -1)) {
        luaL_error(L, "Not a string but %s", luaL_typename(L, lua_type(L, -1)));
    }
    const char *s = lua_tostring(L, -1);
    if (strlen8to16(s) == 1) {
        char16_t ret;
        strcpy8to16(&ret, s, nullptr);
        lua_pushinteger(L, ret);
    } else luaL_error(L, "the string has more than one char:%s", s);
    return 1;
}

int javaCharString(lua_State *L) {
    int isnum;
    jlong v = lua_tointegerx(L, -1, &isnum);
    if (!isnum)
        luaL_error(L, "Not a integer");
    if (v > UINT16_MAX) luaL_error(L, "Not a char value");
    char16_t s = (char16_t) v;
    char *ret = strndup16to8(&s, 1);
    lua_pushstring(L, ret);
    free(ret);
    return 1;
}

int javaNew(lua_State *L) {
    ScriptContext *context = getContext(L);
    JavaType *type = checkJavaType(L,1);
    AutoJNIEnv env;
    auto component = type->getComponentType(env);
    if (component != nullptr) {
        return newArray(env, L, 2, context, component);
    } else {
        Vector<JavaType *> types;
        Vector<ValidLuaObject> objects;
        readArguments(env, L, context, types, objects, 2);
        JObject obj = JObject(env, type->newObject(env,types, objects));
        if (context->hasErrorPending()) {
            forceRelease(types, objects);
            luaL_error(L, "");
        }
        pushJavaObject(env, L, context, obj.get(),type);
    }
    return 1;
}

int javaNewArray(lua_State *L) {
    ScriptContext *context = getContext(L);
    JavaType *type = checkJavaType(L,1);
    return newArray(AutoJNIEnv(), L, 2, context, type);
}

int newArray(TJNIEnv *env, lua_State *L, int index, ScriptContext *context, JavaType *type) {
    jlong size = 0;
    if (type->isVoid()) {
        luaL_error(L, "Type Error:array for void.class can't be created");
    }
    int isnum;
    size = lua_tointegerx(L, index++, &isnum);
    if (!isnum) {
        luaL_error(L, "Type Error: not a integer but %s", luaL_typename(L, index));
    } else if (size > INT32_MAX || size < 0) {
        luaL_error(L, "integer overflowed");
    }
    int top = lua_gettop(L);
    if (top - index > size) {
        luaL_error(L, "%d elements is too many for an array of size %d", top - index, size);
    }
    Vector<ValidLuaObject> elements;
    elements.reserve(top - index + 1);
    for (; index <= top; ++index) {
        ValidLuaObject object;
        if (!parseLuaObject(env, L, context, index, object)) {
            forceRelease(elements);
            luaL_error(L, "Arg unexpected for array");
        }
        checkLuaType(env, L, type, object);
        elements.push_back(std::move(object));
    }
    JArray ret(env, type->newArray(env,(jint) size, elements));
    if (ret == nullptr) {
        forceRelease(elements);
        luaL_error(L, "");
    }
    pushJavaObject(env, L, context, ret);
    return 1;
}

int javaToJavaObject(lua_State *L) {
    AutoJNIEnv env;
    ScriptContext *context = getContext(L);
    ValidLuaObject luaObject;
    parseLuaObject(env, L, context, 1, luaObject);
    if(luaObject.type==T_OBJECT)
        return 1;
    jobject object = context->luaObjectToJObject(env, std::move(luaObject));
    if (object == INVALID_OBJECT)
        luaL_error(L, "");
    pushJavaObject(env, L, context, JObject(env, object));
    return 1;

}

int javaInstanceOf(lua_State *L) {
    JavaObject *objectRef = checkJavaObject(L,1);
    JavaType *typeRef = checkJavaType(L,2);
    lua_pushboolean(L, AutoJNIEnv()->IsInstanceOf(objectRef->object, typeRef->getType()));
    return 1;
}

int javaSync(lua_State *L) {
    JavaObject *objectRef =  checkJavaObject(L,1);
    AutoJNIEnv env;
    env->MonitorEnter(objectRef->object);
    ScriptContext *context = getContext(L);
    HOLD_JAVA_EXCEPTION(context, {luaL_error(L, "");});
    lua_pushcfunction(L,luaPCallHandler);
    lua_pushvalue(L,2);
    int ret;
    ret = lua_pcall(L, 0, 0, -2);
    env->MonitorExit(objectRef->object);
    HOLD_JAVA_EXCEPTION(context, {luaL_error(L, "");});
    if(unlikely(ret!=LUA_OK)){
        lua_error(L);
    }
    return 0;
}



int javaThrow(lua_State *L) {
    JavaObject *objectRef =  checkJavaObject(L,1);
    if (!AutoJNIEnv()->IsInstanceOf(objectRef->object, throwableType))
        luaL_error(L, "Illegal exception");
    ScriptContext *context = getContext(L);
    context->setPendingException((jthrowable) objectRef->object);
    luaL_error(L, "");
    return 0;
}

void parseTryTable(lua_State *L) {
    lua_pushinteger(L, 1);
    lua_gettable(L, 1);
    lua_getfield(L, 1, "catch");
    int catchIndex = lua_gettop(L);
    if (lua_isfunction(L, catchIndex)) {
        lua_pushstring(L, "all");
        lua_pushvalue(L, catchIndex);
    } else if (lua_istable(L, catchIndex)) {
        lua_pushnil(L);
        while (lua_next(L, catchIndex)) {
            lua_pushvalue(L, -2);//push key for next
        }
    }
    lua_remove(L, catchIndex);
    lua_getfield(L, 1, "finally");
    if (lua_isnil(L, -1))
        lua_pop(L, 1);
    lua_remove(L, 1);//remove table
}

int javaTry(lua_State *L) {
    ScriptContext *context = getContext(L);
    int top = lua_gettop(L);
    if (top == 0) luaL_error(L, "No args");
    bool noCatch = false;
    int finallyIndex = 0;
    if (top == 1) {
        if (lua_istable(L, 1)) {
            parseTryTable(L);
        }
    }
    if (lua_isnil(L, 1)) {
        luaL_error(L, "No try body");
    }
    SetErrorJMP();
    top = lua_gettop(L);
    Vector<int> catchFuncs;
    int catchAllIndex = 0;
    AutoJNIEnv env;
    if (top == 1) noCatch = true;
    else if (top == 2) {
        noCatch = true;
        finallyIndex = 2;
    } else {
        int i;
        for (i = 2; i <= top - 1; i += 2) {
            //int type=lua_type(L,i);
            if (testType(L, i, TYPE_KEY)) {
                catchFuncs.push_back(i);
            } else if (luaL_isstring(L, i) && strcmp(lua_tostring(L, i), "all") == 0) {
                if (catchAllIndex == 0) catchAllIndex = i;
                else
                    TopErrorHandle("More than one catch all functions");
            } else {
                TopErrorHandle("Not a catch type");
            }
        }
        if (i == top) finallyIndex = top;
    }
    lua_pushcfunction(L, luaPCallHandler);
    int handlerIndex = lua_gettop(L);
    lua_pushvalue(L, 1);
    int ret = lua_pcall(L, 0, 0, handlerIndex);
    if (ret != LUA_OK) {
        if (!testType(L, -1, RETHROW_KEY)) recordLuaError(env, context, L, ret);
        lua_pop(L, 1);
        jthrowable error = context->getPendingJavaError();
        context->setPendingException(nullptr);
        if (!noCatch) {
            for (int idx:catchFuncs) {
                JavaType *type = *(JavaType **) lua_touserdata(L, idx);
                if (env->IsInstanceOf(error, type->getType())) {
#define CALL_CATCH(index)\
                    lua_pushvalue(L,(index)+1);\
                    pushJavaObject(env,L,context,error);\
                    int code=lua_pcall(L,1,0,handlerIndex);\
                    forceRelease(catchFuncs);\
                    if(finallyIndex>0){lua_pushvalue(L,finallyIndex);lua_call(L,0,0);} \
                    if(code!=LUA_OK){\
                        if(env->IsSameObject(context->getPendingJavaError(),error)){\
                            jthrowable* p=(jthrowable*)lua_newuserdata(L,sizeof(jthrowable));\
                            *p=context->getPendingJavaError();\
                            setMetaTable(L, RETHROW_KEY);\
                            return lua_error(L);\
                        } \
                        recordLuaError(env,context,L,code);\
                        goto __ErrorHandle;\
                    }\
                    return 0;
                    CALL_CATCH(idx);
                }
            }
            if (catchAllIndex > 0) {
                CALL_CATCH(catchAllIndex);
            }
        }
        forceRelease(catchFuncs);
        if (finallyIndex > 0) lua_call(L, 0, 0);
        jthrowable *p = (jthrowable *) lua_newuserdata(L, sizeof(jthrowable));
        *p = error;
        setMetaTable(L, RETHROW_KEY);
        lua_error(L);
    }
    forceRelease(catchFuncs);
    if (finallyIndex > 0) lua_call(L, 0, 0);
    return 0;

}

int javaPut(lua_State *L) {
    ScriptContext *context =* (ScriptContext**)lua_touserdata(L,1);
    const char *name = luaL_tolstring(L,2, nullptr);
    AutoJNIEnv env;
    CrossThreadLuaObject object;
    if (!parseCrossThreadLuaObject(env, L, context, 3, object)) {
        const char *s = luaL_tolstring(L, 3, nullptr);
        luaL_error(L, "Invalid object %s with name %s to be pushed to cross thread table", s, name);
    }
    if(object.type==T_NIL){
        context->deleteLuaObject(name);
    } else context->saveLuaObject(object, name);
    return 0;

}

int javaGet(lua_State *L) {
    ScriptContext *context =* (ScriptContext**)lua_touserdata(L,1);

    const char *name = luaL_tolstring(L, 2, nullptr);
    CrossThreadLuaObject *object = context->getLuaObject(name);
    if (object == nullptr||!pushLuaObject(AutoJNIEnv(), L, context, *object)) {
        lua_pushnil(L);
    }
    return 1;

}

int callMethod(lua_State *L) {
    MemberFlag *flag = (MemberFlag *) lua_touserdata(L, FLAG_INDEX);
    ScriptContext* context=flag->context;
    const char *name = lua_tostring(L, NAME_INDEX);
    SetErrorJMP();
    bool isStatic = flag->isStatic;
    JavaObject *objRef = isStatic ? nullptr : (JavaObject *) lua_touserdata(L, OBJ_INDEX);
    JavaType *type = isStatic ? *(JavaType **) lua_touserdata(L, OBJ_INDEX) : objRef->type;
    Vector<JavaType *> types;
    Vector<ValidLuaObject> objects;
    AutoJNIEnv env;
    readArguments(env, L, context, types, objects, 1 + flag->isNotOnlyMethod);
    auto info = type->findMethod(env,FakeString(name), isStatic, types, &objects);
    if (unlikely(info == nullptr)) {
        TopErrorHandle("No matched found for the method %s;%s",type->name(env).str(), name);
    }
    int argCount = objects.size();
    jvalue args[argCount];
    for (int i = argCount - 1; i != -1; --i) {
        ValidLuaObject &object = objects[i];
        JavaType::ParameterizedType &tp = info->params[i];
        args[i] = context->luaObjectToJValue(env, object, tp.rawType,tp.realType);
        if (!tp.rawType->isPrimitive() && args[i].l == INVALID_OBJECT) {
            cleanArgs(args, argCount, objects, env);
            goto __ErrorHandle;
        }
    }
    int retCount = 1;
    auto returnType=info->returnType.rawType;
#define PushResult(jtype, jname, NAME)\
     case JavaType::jtype:{\
        lua_push##NAME(L,isStatic?env->CallStatic##jname##MethodA(type->getType(),info->id\
        ,args):env->CallNonvirtual##jname##MethodA(objRef->object,objRef->type->getType(),info->id,args));\
        break;}
#define PushFloatResult(jtype, jname) PushResult(jtype,jname,number)
#define PushIntegerResult(jtype, jname) PushResult(jtype,jname,integer)

    switch(returnType->getTypeID()){
        PushResult(BOOLEAN, Boolean, boolean)
        PushIntegerResult(INT, Int)
#if LUA_VERSION_NUM >= 503
        PushIntegerResult(LONG, Long)
#else
        case JavaType::LONG:{
           jlong num=isStatic?env->CallStaticLongMethodA(type->getType(),info->id
           ,args):env->CallNonvirtualLongMethodA(objRef->object,objRef->type->getType(),
           info->id,args);
           if(jlong(double(num))!=num){
               Integer64::pushLong(L,num);
           }else {
               lua_pushnumber(L,num);
           }
           break;
        }
#endif
        PushFloatResult(DOUBLE, Double)
        PushFloatResult(FLOAT, Float)
        PushIntegerResult(BYTE, Byte)
        PushIntegerResult(SHORT, Short)
        case JavaType::CHAR:{
            jchar buf;
            if (isStatic) buf = env->CallStaticCharMethodA(type->getType(), info->id, args);
            else
                buf = env->CallNonvirtualCharMethodA(objRef->object, objRef->type->getType(), info->id, args);
            char s[4]={0} ;
            strncpy16to8(s,(const char16_t *) &buf, 1);
            lua_pushstring(L, s);
            break;
        }
        case JavaType::VOID:
            if (isStatic) env->CallStaticVoidMethodA(type->getType(), info->id, args);
            else
                env->CallNonvirtualVoidMethodA(objRef->object, objRef->type->getType(), info->id, args);
            retCount = 0;
            break;
        default:
            JObject object = isStatic ? env->CallStaticObjectMethodA(type->getType(), info->id, args) :
                             env->CallNonvirtualObjectMethodA(objRef->object, objRef->type->getType(),
                                                              info->id, args);
            if (object == nullptr) lua_pushnil(L); else pushJavaObject(env, L, context, object);
            break;
    }
    HOLD_JAVA_EXCEPTION(context, { goto __ErrorHandle;});
    cleanArgs(args, argCount, objects, env);
    return retCount;
}

int getFieldOrMethod(lua_State *L) {
    bool isStatic = isJavaTypeOrObject(L,1);
    JavaObject *obj = isStatic ? nullptr : (JavaObject*) lua_touserdata(L,1);
    JavaType *type = isStatic ? *(JavaType**) lua_touserdata(L,1) : obj->type;
    ScriptContext *context = type->getContext();
    AutoJNIEnv env;
    if (!isStatic) {
        auto component = obj->type->getComponentType(env);
        if (component != nullptr) {
            if (luaL_isstring(L, 2)) {
                const char *s = lua_tostring(L, 2);
                if (unlikely(strcmp(s, "length") != 0)) {
                    luaL_error(L, "Invalid member for an array:%s", s);
                } else {
                    lua_pushinteger(L, env->GetArrayLength((jarray) obj->object));
                }
            } else {
                pushArrayElement(env, L, context, obj, component);
            }
            return 1;
        }
    }
    if (unlikely(!luaL_isstring(L, 2)))
        luaL_error(L, "Invaild type to get a field or method:%s", luaL_typename(L, 2));

    FakeString name(lua_tostring(L, 2));

    /*if(!validJavaName(name))luaL_error(L,"The name is invalid for java use:%s",name);*/
    int fieldCount = type->getFieldCount(env,name, isStatic);
    bool isMethod = type->ensureMethod(env,name, isStatic) != nullptr;
    if (fieldCount == 1 && !isMethod) {
        auto info = type->findField(env,std::move(name), isStatic, nullptr);
        JavaType *fieldType = info->type.rawType;
#define GetField(typeID,jtype, jname, TYPE)\
        case JavaType::typeID:{\
            lua_push##TYPE(L,isStatic?env->GetStatic##jname##Field(type->getType()\
                    ,info->id):env->Get##jname##Field(obj->object,info->id));\
        }
#define GetIntegerField(typeID,jtype, jname) GetField(typeID,jtype,jname,integer)
#define GetFloatField(typeID,jtype, jname) GetField(typeID,jtype,jname,number)

#if LUA_VERSION_NUM >= 503
#define GetInteger64Field() GetIntegerField(LONG,long,Long)
#else
#define GetInteger64Field()\
    case JavaType::LONG:{\
        jlong v=isStatic?env->GetStaticLongField(type->getType()\
                    ,info->id):env->GetLongField(obj->object,info->id);\
        if(int64_t(double(v))!=v)\
            lua_pushnumber(L,v);\
        else Integer64::pushLong(L,v);\
        break;\
    }
#endif


#define PushField()
        switch(fieldType->getTypeID()){\
            GetIntegerField(INT,int,Int)\
            GetIntegerField(BYTE,byte,Byte)\
            GetInteger64Field()\
            GetIntegerField(SHORT,short,Short)\
            GetFloatField(FLOAT,float,Float)\
            GetFloatField(DOUBLE,double,Double)\
            GetField(BOOLEAN,boolean,Boolean,boolean)\
            case JavaType::CHAR:{\
            jchar c=isStatic?env->GetStaticCharField(type->getType(),info->id):env->GetCharField(obj->object,info->id);\
            PushChar(c);\
            }\
            default:{\
            JObject object=isStatic?env->GetStaticObjectField(type->getType(),info->id):env->GetObjectField(obj->object,info->id);\
            if(object==nullptr) lua_pushnil(L);else pushJavaObject(env,L,context,object);\
            }}
        PushField();
    } else {
        if (!isMethod && unlikely(fieldCount == 0)) {
            if(isStatic&&strcmp(name,"class")==0){
                pushJavaObject(env,L,context,type->getType());
                return 1;
            }
            lua_pushfstring(L, "No member is named %s in class %s", name.data(),
                            type->name(env).str());
            lua_error(L);
            return 0;
        }
        pushMember(context, L, isStatic, fieldCount, isMethod);
    }
    return 1;
}

int getObjectLength(lua_State *L) {
    JavaObject *objRef = (JavaObject *)(lua_touserdata(L, 1));
    AutoJNIEnv env;
    if (objRef->type->isArray(env)) lua_pushinteger(L, env->GetArrayLength((jarray) objRef->object));
    else {
        int len = env->CallStaticIntMethod(contextClass, sLength, objRef->object);
        if (unlikely(len == -1))
            luaL_error(L, "The object %s has no length property", luaL_tolstring(L, 1, nullptr));
        else lua_pushinteger(L, len);
    }
    return 1;
}

int getField(lua_State *L) {
    MemberFlag *flag = (MemberFlag *) lua_touserdata(L, FLAG_INDEX);

    const char *name = lua_tostring(L, NAME_INDEX);
    if (unlikely(!flag->isField)) {
        luaL_error(L, "The member %s is not a field", name);
    }
    bool isStatic = flag->isStatic;
    JavaObject *obj = isStatic ? nullptr : (JavaObject *) lua_touserdata(L, OBJ_INDEX);
    JavaType *type = isStatic ? *(JavaType **) lua_touserdata(L, OBJ_INDEX) : obj->type;

    JavaType **fieldTypeRef;
    if (unlikely(flag->isDuplicatedField) && (fieldTypeRef = (JavaType **) testUData(L, 2, TYPE_KEY)) ==
                                   nullptr) {
        luaL_error(L, "The class has duplicated field name %s", name);
    }
    AutoJNIEnv env;
    auto info = type->findField(env,FakeString(name), isStatic,
                                flag->isDuplicatedField ? nullptr : *fieldTypeRef);
    JavaType *fieldType = info->type.rawType;
    PushField();
    return 1;
}

int setField(lua_State *L) {
    MemberFlag *flag = (MemberFlag *) lua_touserdata(L, FLAG_INDEX);
    ScriptContext* context=flag->context;

    const char *name = lua_tostring(L, NAME_INDEX);
    if (unlikely(!flag->isField)) {
        luaL_error(L, "The member %s is not a field", name);
    }
    bool isStatic = flag->isStatic;
    JavaObject *objRef = isStatic ? nullptr : (JavaObject *) lua_touserdata(L, OBJ_INDEX);
    JavaType *type = isStatic ? *(JavaType **) lua_touserdata(L, OBJ_INDEX) : objRef->type;

    JavaType **fieldTypeRef;
    if (unlikely(flag->isDuplicatedField) && (fieldTypeRef = (JavaType **)
            testUData(L, 2, TYPE_KEY)) == nullptr) {
        luaL_error(L, "The class has duplicated field name %s", name);
    }
    AutoJNIEnv env;
    auto info = type->findField(env,FakeString(name), isStatic,
                                flag->isDuplicatedField ? nullptr : *fieldTypeRef);
    JavaType *fieldType = info->type.rawType;
    ValidLuaObject luaObject;
    if (unlikely(!parseLuaObject(env, L, context, 3, luaObject))) {
        luaL_error(L, "Invalid value passed to java as a field with type:%s", luaL_typename(L, 3));
    }
    checkLuaType(env, L, fieldType, luaObject);
#define RawSetField(jname, NAME)({\
        if(isStatic) env->SetStatic##jname##Field(type->getType(),info->id,NAME);\
        else env->Set##jname##Field(objRef->object,info->id,NAME);})
#define SetField(typeID,jtype, jname, NAME)\
    case JavaType::typeID:{\
        RawSetField(jname,(luaObject.NAME));\
    }
#define SetIntegerField(typeID,jtype, jname) SetField(typeID,jtype,jname,integer)
#define SetFloatField(typeID,jtype, jname) SetField(typeID,jtype,jname,number)
#define SET_FIELD()
    switch(fieldType->getTypeID()){\
        SetIntegerField(INT,int,Int)\
        SetField(BOOLEAN,boolean,Boolean,isTrue)\
        SetIntegerField(LONG,long,Long)\
        SetFloatField(FLOAT,float,Float)\
        SetFloatField(DOUBLE,double,Double)\
        SetIntegerField(BYTE,byte,Byte)\
        SetIntegerField(SHORT,short,Short)\
        case JavaType::CHAR:{\
            char16_t  s;\
            strcpy8to16(&s,luaObject.string, nullptr);\
            RawSetField(Char,s);\
        }\
        default:{\
            jvalue v= context->luaObjectToJValue(env,luaObject,fieldType,info->type.realType);\
            RawSetField(Object,v.l);\
            if(luaObject.type==T_FUNCTION||luaObject.type==T_STRING) env->DeleteLocalRef(v.l);\
        }}
    SET_FIELD()
    return 0;
}

int setFieldOrArray(lua_State *L) {
    bool isStatic = isJavaTypeOrObject(L,1);
    JavaObject *objRef = isStatic ? nullptr : (JavaObject*) lua_touserdata(L,1);
    JavaType *type = isStatic ? *(JavaType**) lua_touserdata(L,1) : objRef->type;
    AutoJNIEnv env;
    ScriptContext *context = type->getContext();
    if (!isStatic) {
        JavaType *component = objRef->type->getComponentType(env);
        if (component != nullptr) {
            type = component;
            int isnum;
            jlong index = lua_tointegerx(L, 2, &isnum);
            if (unlikely(!isnum))
                luaL_error(L, "Invalid Value to set a array:%s", luaL_tolstring(L, 2, nullptr));
            if (index < 0 || index > INT32_MAX) luaL_error(L, "Index out of range:%lld", index);
            ValidLuaObject luaObject;
            parseLuaObject(env, L, context, 3, luaObject);
            checkLuaType(env, L, type, luaObject);

#define RAW_SET_ARR(jtype, jname, Ref) env->Set##jname##ArrayRegion((j##jtype##Array) objRef->object, (jsize) index, 1,Ref)
#define SET_ARR(typeID,jtype, jname, NAME) \
            case JavaType::typeID:{ j##jtype* ref=(j##jtype*) &luaObject.NAME;RAW_SET_ARR(jtype,jname,ref);break;}
#define SET_INTEGER_ARR(typeID,jtype, jname) SET_ARR(typeID,jtype,jname,integer)
#define SET_FLOAT_ARR(typeID,jtype, jname) SET_ARR(typeID,jtype,jname,number)
            switch (type->getTypeID()){
                SET_INTEGER_ARR(BYTE,byte, Byte)
                SET_INTEGER_ARR(SHORT,short, Short)
                SET_INTEGER_ARR(INT,int, Int)
                SET_INTEGER_ARR(LONG,long, Long)
                SET_FLOAT_ARR(FLOAT,float, Float)
                SET_FLOAT_ARR(DOUBLE,double, Double)
                SET_ARR(BOOLEAN,boolean, Boolean, isTrue)
                case JavaType::CHAR:{
                    char16_t s[strlen8to16(luaObject.string)];
                    strcpy8to16(s,luaObject.string, nullptr);
                    RAW_SET_ARR(char, Char, (jchar*)s);}
                default: {
                    jobject v = context->luaObjectToJValue(env, luaObject, type).l;
                    if (unlikely(v == INVALID_OBJECT)) lua_error(L);
                    env->SetObjectArrayElement((jobjectArray) objRef->object, (jsize) index, v);
                    cleanArg(env, v, luaObject.shouldRelease);
                }
            }
            return 0;
        }
    }
    if (unlikely(!luaL_isstring(L, 2)))
        luaL_error(L, "Invalid index for a field member:%s",
                   luaL_tolstring(L, 2, NULL));
    const FakeString name(lua_tostring(L, 2));
    int fieldCount = type->getFieldCount(env,name, isStatic);
    if (fieldCount <= 0) luaL_error(L,"No such field");
    if (fieldCount > 1) luaL_error(L,"The name %s repsents not only one field", name.data());
    auto info = type->findField(env,name, isStatic, nullptr);
    JavaType *fieldType = info->type.rawType;
    ValidLuaObject luaObject;
    if (unlikely(!parseLuaObject(env, L, context, 3, luaObject))) {
        luaL_error(L,"Invalid value passed to java as a field with type:%s",
                       luaL_tolstring(L, 3, NULL));
    }
    checkLuaType(env, L, fieldType, luaObject);

    SET_FIELD();
    return 0;


}

int objectEquals(lua_State *L) {
    JavaObject *ob1 = (JavaObject *) testUData(L, 1, OBJECT_KEY);
    JavaObject *ob2 = (JavaObject *) testUData(L, 2, OBJECT_KEY);
    if (ob1 == nullptr || ob2 == nullptr) {
        lua_pushboolean(L, false);
    } else lua_pushboolean(L, AutoJNIEnv()->IsSameObject(ob1->object, ob2->object));
    return 1;
}

int concatString(lua_State *L) {
    size_t len1 = 0;
    size_t len2 = 0;
    const char *s1 = luaL_tolstring(L, 1, &len1);
    const char *s2 = luaL_tolstring(L, 2, &len2);
    lua_pushfstring(L,"%s%s", s1,s2);
    return 1;
}

int javaTypeToString(lua_State *L) {
    JavaType *type = *(JavaType**)lua_touserdata(L,1);
    AutoJNIEnv env;
    lua_pushfstring(L,"Java Type:%s", type->name(env).str());
    return 1;
}

int javaObjectToString(lua_State *L) {
    JavaObject *ob = (JavaObject*)lua_touserdata(L,1);
    AutoJNIEnv env;
    JString str = env->CallObjectMethod(ob->object, objectToString);
    lua_pushstring(L, str);
    return 1;
}

int funcWriter(lua_State *, const void *p, size_t sz, void *ud) {
    Vector<char> *holder = (Vector<char> *) ud;
    for (int i = 0; i < sz; ++i) {
        holder->push_back(((const char *) p)[i]);
    }
    return 0;
}

FuncInfo *saveLuaFunction(JNIEnv *env, lua_State *L, ScriptContext *context, int funcIndex) {
    typedef Vector<std::pair<int, FuncInfo *>> MY_VECTOR;
    static thread_local MY_VECTOR *parsedFuncs;
    bool isOwner = false;
    if (parsedFuncs == nullptr) {
        parsedFuncs = new MY_VECTOR();
        isOwner = true;
    }
    for (auto &pair:*parsedFuncs) {
        if (lua_rawequal(L, -1, pair.first))
            return pair.second;
    }
    lua_pushvalue(L, funcIndex);
    bool isCFunc = lua_iscfunction(L, -1) == 1;
    FuncInfo *ret;
    if (!isCFunc) {
        Vector<char> holder;
#if LUA_VERSION_NUM >= 503
#define STRIP ,1
#else
#define STRIP
#endif
        if (unlikely(lua_dump(L, funcWriter, &holder STRIP) != 0)) {
            lua_pop(L, 1);
            return nullptr;//unknown error;
        }
#undef STRIP
        ret = new FuncInfo(Array<char>(holder));
    } else {
        ret = new FuncInfo(lua_tocfunction(L, -1));
    }
    lua_pop(L, 1);
    parsedFuncs->push_back(std::make_pair(funcIndex, ret));
    Vector<CrossThreadLuaObject> upvalues;

    for (int i = 1;; ++i) {
        const char* name;
        if ((name=lua_getupvalue(L, funcIndex, i)) == NULL)
            break;
        CrossThreadLuaObject object;
#if LUA_VERSION_NUM>=502
        if(strcmp(name,"_ENV")==0)
            ret->globalIndex=i;
        else
#endif
            parseCrossThreadLuaObject(env, L, context, -1, object);
        upvalues.push_back(std::move(object));
        lua_pop(L,1);
    }
    ret->setUpValues(Array<CrossThreadLuaObject>(std::move(upvalues)));

    ret->setImport(context->getImport());
    if (isOwner) {
        delete parsedFuncs;
        parsedFuncs = nullptr;
    }
    return ret;
}

void loadLuaFunction(TJNIEnv *env, lua_State *L, const FuncInfo *info, ScriptContext *context) {
    typedef Map<const FuncInfo *, int> MY_MAP;
    static thread_local MY_MAP *loadedFuncs;
    bool isOwner = false;
    if (loadedFuncs == nullptr) {
        loadedFuncs = new MY_MAP();
        isOwner = true;
    }
    const auto &iter = loadedFuncs->find(info);
    if (iter != loadedFuncs->end()) {
        lua_pushvalue(L, (*iter).second);
        return;
    }
    if (info->isCFunc) {
        uint32_t count = info->getUpValues().size();
        for (int i = 0; i < count; ++i) {
            lua_pushnil(L);
        }
        lua_pushcclosure(L, info->cFunc, count);
    } else {
        luaL_loadbuffer(L, &info->funcData.at(0), info->funcData.size(), "");
    }
    int funcIndex = lua_gettop(L);
    loadedFuncs->emplace(info, funcIndex);
    auto &upvalues = info->getUpValues();

    for (int i = upvalues.size() - 1; i >= 0; --i) {
        CrossThreadLuaObject &luaObject = upvalues[i];
        if (!pushLuaObject(env, L, context, luaObject))continue;
        lua_setupvalue(L, funcIndex, i + 1);//start at the second;
    }
#if LUA_VERSION_NUM>502
    if(info->globalIndex>0){
        lua_rawgeti(L,LUA_REGISTRYINDEX,LUA_RIDX_GLOBALS);
        lua_setupvalue(L,funcIndex,info->globalIndex);
    }
#endif
    if (isOwner) {
        delete loadedFuncs;
        loadedFuncs = nullptr;
    }
}

jobject LazyTable::asInterface(TJNIEnv *env, ScriptContext *context, JavaType *main) {
    Vector<JavaType *> interfaces;
    Vector<std::unique_ptr<BaseFunction>> luaFuncs;
    Vector<JObject> agentMethods;
    if (!readProxyMethods(env, L, context, interfaces, main, luaFuncs, agentMethods))
        return INVALID_OBJECT;
    return context->proxy(env, main, nullptr, agentMethods, luaFuncs);
}

LuaTable<ValidLuaObject> *LazyTable::getTable(TJNIEnv *env, ScriptContext *context) {
    if (table != nullptr)
        return table;
    lua_pushvalue(L, index);
    lua_rawget(L, LUA_REGISTRYINDEX);
    LuaTable<ValidLuaObject> *luaTable;
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaTable = new LuaTable<ValidLuaObject>();
        int len =(int) lua_rawlen(L, index);
        luaTable->get().reserve(len);
        lua_pushvalue(L, index);
        lua_pushlightuserdata(L, luaTable);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_pushnil(L);
        while (lua_next(L, index)) {
            ValidLuaObject key;
            ValidLuaObject value;
            bool ok = parseLuaObject(env, L, context, -2, key) &&
                      parseLuaObject(env, L, context, -1, value);
            lua_pop(L, 1);
            if (ok)
                luaTable->get().push_back({std::move(key), std::move(value)});
            else {
                lua_pushvalue(L, index);
                lua_pushnil(L);
                lua_rawset(L, LUA_REGISTRYINDEX);
                delete luaTable;
                return nullptr;
            }
        }
        lua_pushvalue(L, index);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    } else {
        luaTable = static_cast<LuaTable<ValidLuaObject> *>(lua_touserdata(L, -1));
        lua_pop(L, -1);
    }
    table = luaTable;
    return luaTable;
}


void ScriptContext::pushAddedObject(TJNIEnv *env, lua_State *L, const char *name, const char *methodName,
                                    jobject obj) {
    if (methodName == nullptr || methodName[0] == 0) {
        if (obj == nullptr) {
            lua_pushnil(L);
        } else {
            pushJavaObject(env, L, this, obj);
        }
        lua_setglobal(L, name);
    } else if (obj != nullptr) {
        int top = lua_gettop(L);
        bool isStatic = false;
        if (env->IsInstanceOf(obj, classType)) {
            JavaType *type = ensureType(env, (jclass) obj);
            *(JavaType **) lua_newuserdata(L, sizeof(JavaType *)) = type;
            isStatic = true;
        } else {
            pushJavaObject(env, L, this, obj);
        }
        lua_pushstring(L, methodName);
        pushMember(this, L, isStatic, 0, true);

        lua_setglobal(L, name);
        lua_settop(L, top);
    }
}

static void registerNativeMethods(JNIEnv *env) {
    jclass scriptClass = env->FindClass("com/oslorde/luadroid/ScriptContext");
    env->RegisterNatives(scriptClass, nativeMethods,
                         sizeof(nativeMethods) / sizeof(JNINativeMethod));
    env->DeleteLocalRef(scriptClass);
}

jint JNI_OnLoad(JavaVM *vm, void *) {
    ::vm = vm;
    JNIEnv *env;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_4) != JNI_OK) {
        return -1;
    }
    registerNativeMethods(env);
    return JNI_VERSION_1_4;
}

jlong nativeOpen(TJNIEnv *env, jobject object, jboolean importAll) {
    ScriptContext *context = new ScriptContext(env, object, importAll);
    requireLogger([context, env](const char *data, bool isErr) {
        context->writeLog(data, isErr);
    });
    return reinterpret_cast<long>(context);
}

void registerLogger(TJNIEnv *env, jclass, jlong ptr, jobject out, jobject err) {
    ScriptContext *context = (ScriptContext *) ptr;
    if (context != nullptr) {
        context->registerLogger(env, out, err);
    };
}

void nativeClose(JNIEnv *, jclass, jlong ptr) {
    ScriptContext *context = (ScriptContext *) ptr;
    if (context != nullptr) {
        delete context;
        dropLogger();
    }
}

void nativeClean(JNIEnv *, jclass, jlong ptr) {
    ScriptContext *context = (ScriptContext *) ptr;
    if (context != nullptr)context->clean();
}

void releaseFunc(JNIEnv *, jclass, jlong ptr) {
    BaseFunction *info = (BaseFunction *) ptr;
    if (--info->javaRefCount == 0) {
        delete info;
    }
}

jint getClassType(TJNIEnv * env, jclass, jlong ptr,jclass clz) {
    ScriptContext *context = (ScriptContext *) ptr;
    return context->ensureType(env,clz)->getTypeID();
}

void addJavaObject(TJNIEnv *env, jclass, jlong ptr, jstring _name, jobject obj, jboolean local) {
    ScriptContext *context = (ScriptContext *) ptr;
    JString name(env, _name);
    context->addJavaObject(name, nullptr, env->NewGlobalRef(obj), local);
    name.invalidate();
}

void addJavaMethod(TJNIEnv *env, jclass, jlong ptr, jstring jname, jstring method, jobject jinst,
                   jboolean local) {
    if (jinst == nullptr) return;
    ScriptContext *context = (ScriptContext *) ptr;
    JString name(env, jname);
    JString methodName(env, method);
    context->addJavaObject(name, methodName, env->NewGlobalRef(jinst), local);
    name.invalidate();
    methodName.invalidate();
}

jlong compile(TJNIEnv *env, jclass, jlong ptr, jstring script, jboolean isFile) {
    ScriptContext *context = (ScriptContext *) ptr;
    auto L = context->getLua();
    JString s(env, script);
    int ret;
    if (isFile)ret = luaL_loadfile(L, s);
    else ret = luaL_loadstring(L, s);
    jlong retVal = 0;
    if (ret != LUA_OK) {
        context->setPendingException(env, "Failed to load");
        recordLuaError(env, context, L, ret);
        context->throwToJava();
    } else {
        FuncInfo *info = saveLuaFunction(env, L, context, -1);
        info->javaRefCount++;
        retVal = reinterpret_cast<jlong >(info);
    }
    lua_pushcfunction(L, luaFullGC);
    lua_pcall(L, 0, 0, 0);
    s.invalidate();
    return retVal;
}

jobjectArray runScript(TJNIEnv *env, jclass, jlong ptr, jobject script, jboolean isFile,
                       jobjectArray args) {
    ScriptContext *context = (ScriptContext *) ptr;
    if (_setjmp(errorJmp)) {
        if (context->hasErrorPending()) {
            context->throwToJava();
        }
        return nullptr;
    }
    Import myIMport;
    auto import = context->changeImport(&myIMport);
    auto L = context->getLua();
    auto top = lua_gettop(L);
    int ret;
    int argCount;
    jobjectArray result = nullptr;
    lua_pushcfunction(L, luaPCallHandler);
    int handlerIndex = lua_gettop(L);
    if (env->IsInstanceOf(script, stringType)) {
        JString s(env, static_cast<jstring>(script));
        if (isFile)ret = luaL_loadfile(L, s);
        else {
            ret = luaL_loadstring(L, s);
        }
        s.invalidate();
    } else {
        FuncInfo *info = reinterpret_cast<FuncInfo *>(env->CallLongMethod(script, longValue));
        ret = luaL_loadbuffer(L, &info->funcData[0], info->funcData.size(), "");
    }
    if (unlikely(ret != LUA_OK)) {
        context->setPendingException(env, "Failed to load");
        recordLuaError(env, context, L, ret);
        goto over;
    }
    argCount = args != nullptr ? 0 : env->GetArrayLength(args);
    for (int i = 0; i < argCount; ++i) {
        JObject object(env->GetObjectArrayElement(args, i));
        pushJavaObject(env, L, context, object);
    }
    ret = lua_pcall(L, argCount, LUA_MULTRET, handlerIndex);
    if (unlikely(ret != LUA_OK)) {
        recordLuaError(env, context, L, ret);
        goto over;
    }
    {
        int resultSize = lua_gettop(L) - handlerIndex;
        if (resultSize > 0) {
            result = env->asJNIEnv()->NewObjectArray(resultSize, context->ObjectClass->getType(),
                                                     NULL);
            for (int i = resultSize - 1; i >= 0; --i) {
                ValidLuaObject object;
                parseLuaObject(env, L, context, handlerIndex + i + 1, object);
                jobject value = context->luaObjectToJObject(env, std::move
                        (object));
                if (value != INVALID_OBJECT)
                    env->SetObjectArrayElement(result, i, JObject(env, value).get());
            }
        }
    }
    over:
    context->changeImport(import);
    lua_settop(L, top);
    lua_pushcfunction(L, luaFullGC);
    lua_pcall(L, 0, 0, 0);

    if (context->hasErrorPending()) {
        context->throwToJava();
    }
    return result;
}

jobject invokeLuaFunction(TJNIEnv *env, jclass, jlong ptr,
                          jboolean isInterface,
                          jlong funcRef, jobject proxy, jintArray argTypes, jobjectArray args) {
    ScriptContext *context = (ScriptContext *) ptr;
    lua_State *L = context->getLua();
    if (_setjmp(errorJmp)) {
        if (context->hasErrorPending())
            context->throwToJava();
        return 0;
    }
    lua_pushcfunction(L, luaPCallHandler);
    int handlerIndex = lua_gettop(L);
    Import *old;
    if (!reinterpret_cast<BaseFunction *>(funcRef)->isLocal()) {
        FuncInfo *funcInfo = (FuncInfo *) funcRef;
        old = context->changeImport(funcInfo->getImport());
        loadLuaFunction(env, L, funcInfo, context);
    } else {
        old = context->getImport();
        LocalFunctionInfo *info = reinterpret_cast<LocalFunctionInfo *>(funcRef);
        lua_rawgetp(L, LUA_REGISTRYINDEX,info);
        if (unlikely(lua_isnil(L, -1))) {
            context->setPendingException(
                    env, "Local Function must run in the given thread it's extracted from");
            context->throwToJava();
            return 0;
        }
    }
    int len = env->GetArrayLength(argTypes);
    jint *arr = env->GetIntArrayElements(argTypes, nullptr);
    for (int i = 0; i < len; ++i) {
        switch (arr[i]) {
            case 0: {//char
                JObject character = env->GetObjectArrayElement(args, i);
                char16_t c = env->CallCharMethod(character, charValue);
                char charStr[4]={0} ;
                strncpy16to8(charStr,&c, 1);
                lua_pushstring(L, charStr);
                break;
            }
            case 1: {//boolean
                JObject boolean = env->GetObjectArrayElement(args, i);
                jboolean b = env->CallBooleanMethod(boolean, booleanValue);
                lua_pushboolean(L, b);
                break;
            }
            case 2: {//integer
                JObject integer = env->GetObjectArrayElement(args, i);
                jlong intValue = env->CallLongMethod(integer, longValue);
                lua_pushinteger(L, intValue);
                break;
            }
            case 3: {//double
                JObject box = env->GetObjectArrayElement(args, i);
                jdouble floatValue = env->CallDoubleMethod(box, doubleValue);
                lua_pushnumber(L, floatValue);
                break;
            }
            default: {//object
                JObject obj = env->GetObjectArrayElement(args, i);
                if (obj == nullptr) {
                    lua_pushnil(L);
                } else pushJavaObject(env, L, context, obj);
                break;
            }
        }
    }
    env->ReleaseIntArrayElements(argTypes, arr, JNI_ABORT);

    pushJavaObject(env, L, context, proxy);
    if (!isInterface) {
        pushJavaObject(env, L, context, proxy, context->ensureType(
                env, env->GetSuperclass(env->GetObjectClass(proxy)))) ;
    }
    len += 1 + !isInterface;
    int err = lua_pcall(L, len, LUA_MULTRET, handlerIndex);
    jobject ret = nullptr;
    int retCount;
    if (err != LUA_OK)recordLuaError(env, context, L, err);
    else if ((retCount = lua_gettop(L) - handlerIndex) != 0) {
        bool multiRet =
                env->IsInstanceOf(proxy, context->FunctionType(env)->getType()) && isInterface;
        if (multiRet) {
            JObjectArray result(
                    env->NewObjectArray(retCount, context->ObjectClass->getType(), NULL));
            for (int i = handlerIndex + 1, top = lua_gettop(L), j = 0; i <= top; ++i, ++j) {
                ValidLuaObject object;
                parseLuaObject(env, L, context, i, object);
                jobject value = context->luaObjectToJObject(env, std::move(object));
                if (value != INVALID_OBJECT)
                    env->SetObjectArrayElement(result, j, JObject(env, value));
            }
            ret = result.invalidate();
        } else {
            ValidLuaObject object;
            parseLuaObject(env, L, context, -1, object);
            ret = context->luaObjectToJObject(env, std::move(object));
            if (ret == INVALID_OBJECT) ret = nullptr;
        }
    }
    context->changeImport(old);
    lua_settop(L, handlerIndex - 1);
    lua_pushcfunction(L, luaFullGC);
    lua_pcall(L, 0, 0, 0);
    if (context->hasErrorPending())
        context->throwToJava();
    return ret;
}

bool validJavaName(const char *s) noexcept {
    if ((int8_t) *s >= 0 && !isalpha(*s) && *s != '$' && *s != '_') return false;//check for the
    while (*++s != 0) {
        if ((int8_t) *s < 0) continue;
        if (!isalnum(*s) && *s != '$' && *s != '_') return false;
    }
    return true;
}

