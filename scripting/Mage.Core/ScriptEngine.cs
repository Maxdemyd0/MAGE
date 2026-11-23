using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Mage.Interop
{
    /// <summary>
    /// Managed side of the scripting host. The engine resolves <see cref="Initialize"/> through
    /// hostfxr (<c>hdt_get_function_pointer</c>) and everything else through the returned
    /// <see cref="ManagedCallbacks"/>. No exception is ever allowed to cross into native code.
    /// </summary>
    public static unsafe class ScriptEngine
    {
        private static readonly Dictionary<uint, Entity> s_instances = new();
        private static readonly List<Entity> s_updateList = new();
        private static readonly HashSet<uint> s_faulted = new();
        private static bool s_listDirty = true;
        private static string s_scriptDirectory = "";

        [UnmanagedCallersOnly]
        public static int Initialize(NativeBindings* bindings, ManagedCallbacks* callbacks)
        {
            try
            {
                if (bindings == null || callbacks == null) return -1;
                if (bindings->StructSize != (uint)sizeof(NativeBindings)) return -2;
                if (bindings->Version != InternalCalls.InteropVersion) return -3;

                InternalCalls.Bind(*bindings);

                // Resolve sibling assemblies (GameScripts.dll -> Mage.Core.dll, NuGet deps) from the Scripts folder.
                s_scriptDirectory = Path.GetDirectoryName(typeof(ScriptEngine).Assembly.Location) ?? AppContext.BaseDirectory;
                AssemblyLoadContext.Default.Resolving += ResolveFromScriptDirectory;

                callbacks->CreateScript = &CreateScript;
                callbacks->DestroyScript = &DestroyScript;
                callbacks->OnUpdate = &OnUpdate;
                callbacks->OnFixedUpdate = &OnFixedUpdate;
                callbacks->Shutdown = &Shutdown;

                Log.Info($"Mage.Core {typeof(ScriptEngine).Assembly.GetName().Version} running on .NET {Environment.Version} ({RuntimeInformation.FrameworkDescription})");
                return 0;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine($"[Mage.Core] Initialize failed: {e}");
                return -100;
            }
        }

        private static Assembly? ResolveFromScriptDirectory(AssemblyLoadContext context, AssemblyName name)
        {
            string candidate = Path.Combine(s_scriptDirectory, name.Name + ".dll");
            return File.Exists(candidate) ? context.LoadFromAssemblyPath(candidate) : null;
        }

        [UnmanagedCallersOnly]
        private static int CreateScript(uint entityId, byte* classNameUtf8)
        {
            try
            {
                string className = Marshal.PtrToStringUTF8((IntPtr)classNameUtf8) ?? string.Empty;
                Type? type = FindScriptType(className);
                if (type == null)
                {
                    Log.Error($"Script class '{className}' not found (must derive from Mage.Entity)");
                    return 0;
                }

                var instance = (Entity)Activator.CreateInstance(type, nonPublic: true)!;
                instance.ID = entityId;
                if (s_instances.TryGetValue(entityId, out Entity? previous)) SafeInvoke(previous, static e => e.OnDestroy());
                s_instances[entityId] = instance;
                s_listDirty = true;

                SafeInvoke(instance, static e => e.OnCreate());
                return 1;
            }
            catch (Exception e)
            {
                Log.Error($"CreateScript failed: {e}");
                return 0;
            }
        }

        [UnmanagedCallersOnly]
        private static void DestroyScript(uint entityId)
        {
            try
            {
                if (s_instances.Remove(entityId, out Entity? instance))
                {
                    s_listDirty = true;
                    SafeInvoke(instance, static e => e.OnDestroy());
                }
                s_faulted.Remove(entityId);
            }
            catch (Exception e)
            {
                Log.Error($"DestroyScript failed: {e}");
            }
        }

        [UnmanagedCallersOnly]
        private static void OnUpdate(float dt)
        {
            Time.DeltaTime = dt;
            Time.Elapsed += dt;
            Time.FrameCount++;
            foreach (Entity e in UpdateList()) SafeInvoke(e, x => x.OnUpdate(dt));
        }

        [UnmanagedCallersOnly]
        private static void OnFixedUpdate(float fixedDt)
        {
            Time.FixedDeltaTime = fixedDt;
            foreach (Entity e in UpdateList()) SafeInvoke(e, x => x.OnFixedUpdate(fixedDt));
        }

        [UnmanagedCallersOnly]
        private static void Shutdown()
        {
            try
            {
                foreach (Entity e in UpdateList()) SafeInvoke(e, static x => x.OnDestroy());
                s_instances.Clear();
                s_updateList.Clear();
                Log.Info("Mage.Core scripting shut down");
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e);
            }
            InternalCalls.Unbind();
        }

        private static List<Entity> UpdateList()
        {
            if (s_listDirty)
            {
                s_updateList.Clear();
                s_updateList.AddRange(s_instances.Values);
                s_listDirty = false;
            }
            return s_updateList;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static void SafeInvoke(Entity entity, Action<Entity> action)
        {
            if (s_faulted.Contains(entity.ID)) return;
            try
            {
                action(entity);
            }
            catch (Exception e)
            {
                // Disable a script after its first unhandled exception instead of spamming every frame.
                s_faulted.Add(entity.ID);
                Log.Error($"{entity.GetType().FullName} (entity {entity.ID}) threw and was disabled: {e}");
            }
        }

        private static Type? FindScriptType(string name)
        {
            foreach (Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                Type? t = asm.GetType(name, throwOnError: false);
                if (t != null && typeof(Entity).IsAssignableFrom(t) && !t.IsAbstract) return t;
            }
            // Fallback: match by short type name.
            foreach (Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                Type[] types;
                try { types = asm.GetTypes(); }
                catch (ReflectionTypeLoadException ex) { types = Array.FindAll(ex.Types, x => x != null)!; }
                foreach (Type t in types)
                    if (t.Name == name && typeof(Entity).IsAssignableFrom(t) && !t.IsAbstract) return t;
            }
            return null;
        }
    }
}
