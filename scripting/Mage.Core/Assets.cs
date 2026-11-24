// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using Mage.Interop;

namespace Mage
{
    public enum ModelPhysics { None = 0, Static = 1, Dynamic = 2 }
    public enum TextureSlot { BaseColor = 0, MetallicRoughness = 1, Normal = 2, Emissive = 3 }

    /// <summary>Imported files. Paths are relative to the asset folder (or absolute).</summary>
    public static class Assets
    {
        /// <summary>Spawns a .gltf/.glb/.fbx/.obj model. Returns <see cref="Physics.NoEntity"/> if it cannot be loaded.</summary>
        public static uint SpawnModel(string path, Vector3 position, Quaternion rotation, Vector3 scale, ModelPhysics physics = ModelPhysics.None) =>
            InternalCalls.SpawnModel(path, position, rotation, scale, (int)physics);

        /// <summary>Sets a texture on a primitive entity; an empty path clears it.</summary>
        public static bool SetTexture(uint entity, string path, TextureSlot slot = TextureSlot.BaseColor) =>
            InternalCalls.SetEntityTexture(entity, (int)slot, path);
    }

    /// <summary>Clips of models that have a skeleton or animated parts.</summary>
    public static class Animator
    {
        /// <summary>Crossfades to a clip. Repeating the call for a looping clip does not restart it.</summary>
        /// <returns>False if the entity is not animated or has no such clip.</returns>
        public static bool Play(uint entity, string clip, float fade = 0.2f, bool loop = true, float speed = 1f) =>
            InternalCalls.PlayAnimation(entity, clip, fade, loop, speed);

        /// <summary>Freezes the current pose.</summary>
        public static void Stop(uint entity) => InternalCalls.StopAnimation(entity);

        /// <summary>Adds the clips of another file made for the same skeleton (for example a Mixamo animation).</summary>
        /// <returns>Number of clips added, -1 on failure.</returns>
        public static int LoadClips(uint entity, string path) => InternalCalls.LoadAnimations(entity, path);

        /// <summary>False when stopped, when a one-shot clip has ended, or when the entity is not animated.</summary>
        public static bool IsPlaying(uint entity) => InternalCalls.AnimationState(entity, out _, out _) == 1;

        /// <summary>Playback position and length in seconds.</summary>
        public static bool GetTime(uint entity, out float time, out float duration) => InternalCalls.AnimationState(entity, out time, out duration) >= 0;

        /// <param name="inPlace">Remove the horizontal travel of clips, so that the script moves the character.</param>
        public static void Configure(uint entity, float speed, bool inPlace) => InternalCalls.SetAnimation(entity, speed, inPlace);

        /// <summary>World position and rotation of a bone, for attaching objects or placing effects.</summary>
        public static bool GetBone(uint entity, string bone, out Vector3 position, out Quaternion rotation) =>
            InternalCalls.BonePosition(entity, bone, out position, out rotation);
    }

    /// <summary>Sound playback (.wav .mp3 .flac .ogg).</summary>
    public static class Audio
    {
        /// <param name="position">World position for 3D sound, or null for flat playback.</param>
        /// <returns>Sound handle, 0 on failure.</returns>
        public static uint Play(string path, float volume = 1f, float pitch = 1f, bool loop = false, Vector3? position = null) =>
            InternalCalls.PlayAudio(path, volume, pitch, loop, position);

        public static void Stop(uint sound) => InternalCalls.StopAudio(sound);
    }

    /// <summary>On-screen text. Call Draw every frame from OnUpdate.</summary>
    public static class Text
    {
        /// <summary>Loads a .ttf/.otf font. Returns 0 on failure.</summary>
        public static int LoadFont(string path, float size = 24f) => InternalCalls.LoadFont(path, size);

        /// <param name="rgba">Colour as 0xRRGGBBAA.</param>
        public static void Draw(string text, float x, float y, float size = 24f, uint rgba = 0xFFFFFFFF, int font = 0) =>
            InternalCalls.DrawLabel(text, x, y, size, rgba, font);
    }
}
