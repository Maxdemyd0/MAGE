// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
using System;
using Mage.Interop;

namespace Mage
{
    public enum PathResult { None = 0, Partial = 1, Complete = 2 }

    public enum NavStatus
    {
        None = -1,
        Idle = 0,
        Moving = 1,
        Arrived = 2,
        /// <summary>Went as far as possible; the target cannot be reached.</summary>
        Blocked = 3,
        /// <summary>No navigation mesh here or at the target yet (still streaming).</summary>
        Waiting = 4,
    }

    /// <summary>Queries on the navigation mesh around the player.</summary>
    public static class Navigation
    {
        /// <param name="points">Corners of the path. Partial paths end as close as possible to the target.</param>
        public static PathResult FindPath(Vector3 from, Vector3 to, out Vector3[] points)
        {
            var buffer = new Vector3[256];
            int n = InternalCalls.NavFindPath(from, to, buffer, out int result);
            points = buffer.AsSpan(0, Math.Min(n, buffer.Length)).ToArray();
            return (PathResult)result;
        }

        /// <summary>A random walkable point that can be reached from the centre.</summary>
        public static bool RandomPoint(Vector3 center, float radius, out Vector3 point) => InternalCalls.NavRandomPoint(center, radius, out point);

        /// <summary>The walkable point next to a position. False if there is none within 2 m.</summary>
        public static bool NearestPoint(Vector3 position, out Vector3 point) => InternalCalls.NavNearestPoint(position, out point);
    }

    /// <summary>Walks a character entity along paths, around other characters and vehicles.</summary>
    public sealed class NavAgent
    {
        public uint EntityId { get; }

        public NavAgent(uint entity) { EntityId = entity; }
        public NavAgent(Entity owner) : this(owner.ID) { }

        /// <summary>False if the entity is not a character.</summary>
        public bool MoveTo(Vector3 target, float speed = 1.5f, float stopDistance = 0.4f) => InternalCalls.NavMoveTo(EntityId, target, speed, stopDistance);
        public void Stop() => InternalCalls.NavStop(EntityId);
        public NavStatus Status => (NavStatus)InternalCalls.NavStatus(EntityId);
    }
}
