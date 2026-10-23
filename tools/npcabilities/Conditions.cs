// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.UObject;
using Mapc;

namespace NpcAbilities;

/// <summary>
/// The conditions of a state tree, one by one: what each tests, which values it tests them against, and where those values come from.  A state's enter conditions are
/// a list; each has an operand (how it joins what came before: the first copies, the rest `And` or `Or`) and an indent change (a positive one opens that many brackets before it,
/// a negative one closes that many after it).  A condition's own settings sit in its instance data, and a setting can be bound to a value the tree has elsewhere (the
/// creature's own actor, the target the hate list names, a task's timer, one of the tree's parameters): the tree's property bindings say which.
///
/// The table says a condition as an atom, one of:
///   T / F            true / false for good (a parameter switched it)
///   in(D) / out(D)   the target is nearer than D / farther than D (cm, from surface to surface)
///   cin(D) / cout(D) the centres of the creature and its target are nearer than D / not (cm, in 3D)
///   ang(A)           the target is within A degrees of the creature's facing
///   cd(NAME)         the cooldown of that name is over
///   rand(P)          a fresh roll under P
///   hp(V) / nhp(V)   the creature's own health is under V of its maximum / is not
///   fc(OP,V)         the seconds since the creature's fight began compare as OP (eq ne lt le gt ge) to V
///   los / nlos       the creature sees its target / does not
///   ?NAME            a condition this tool cannot say (the task is left out)
/// </summary>
public static partial class Reader
{
    /// <summary>A tree and the arrays of it the conditions need.</summary>
    sealed class TreeInfo
    {
        public FStructFallback[] States = Array.Empty<FStructFallback>(), Nodes = Array.Empty<FStructFallback>(), Shared = Array.Empty<FStructFallback>(), Defaults = Array.Empty<FStructFallback>();
        public FStructFallback[] Batches = Array.Empty<FStructFallback>(), Paths = Array.Empty<FStructFallback>(), Sources = Array.Empty<FStructFallback>();
    }

    /// <summary>Where a bound setting gets its value.</summary>
    sealed class Source
    {
        public string Kind = "", Class = "", Name = "", Path = "";   // Context / Parameter / Evaluator / Task / PropertyFunction ...; the struct's class and name; the property inside it
        public bool Global;
        public int State = -1;
        public override string ToString() => Kind + ":" + (Class.Length > 0 ? Class + ":" : "") + Name + (Path.Length > 0 ? "." + Path : "");
    }

    /// <summary>One condition as the table says it.</summary>
    sealed class Atom
    {
        public string Text = "";
        public string Kind = "";        // dist, ang or cd for a condition the table's own columns carry, else empty
        public double Value;
        public bool Inverse;
        public string Name = "";
        public bool Unknown;
        public string Constant = "";    // "T" or "F"
    }

    static TreeInfo LoadTree(UObject tree)
    {
        var t = new TreeInfo
        {
            States = StructsOf(tree, "States").ToArray(),
            Nodes = Container(tree.Properties, "Nodes"),
            Shared = InstanceStructs(tree, "SharedInstanceData"),
            Defaults = InstanceStructs(tree, "DefaultInstanceData")
        };
        if (tree.TryGetValue(out FStructFallback bindings, "PropertyBindings") && bindings != null)
        {
            t.Batches = StructArray(bindings, "CopyBatches");
            t.Paths = StructArray(bindings, "PropertyPathBindings");
            t.Sources = StructArray(bindings, "SourceStructs");
        }
        return t;
    }

    static FStructFallback[] StructArray(FStructFallback owner, string name)
    {
        var prop = owner.Properties.FirstOrDefault(p => p.Name.Text == name);
        if (prop?.Tag?.GenericValue is not UScriptArray arr) return Array.Empty<FStructFallback>();
        return arr.Properties.Select(x => x.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback f ? f : new FStructFallback()).ToArray();
    }

    static string TextOf(FStructFallback s, string name)
    {
        var v = s.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        if (v == null) return "";
        string text = v is FName n ? n.Text : v is FPackageIndex pi ? (pi.IsNull ? "" : pi.Name) : v.ToString() ?? "";
        int colons = text.LastIndexOf("::", StringComparison.Ordinal);
        return colons >= 0 ? text.Substring(colons + 2) : text;
    }

    static int IndexOf(FStructFallback s, string name)
    {
        var v = s.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        if (v is FScriptStruct ss && ss.StructType is FStructFallback f) return IntOf(f, "Value", IntOf(f, "Index"));
        return -1;
    }

    static string PathText(FStructFallback? p)
    {
        if (p == null) return "";
        var segs = p.Properties.FirstOrDefault(x => x.Name.Text == "Segments")?.Tag?.GenericValue as UScriptArray;
        if (segs == null) return "";
        return string.Join(".", segs.Properties.Select(x => x.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback f ? TextOf(f, "Name") : "?"));
    }

    static FStructFallback? StructOf(FStructFallback s, string name) => s.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue is FScriptStruct ss ? ss.StructType as FStructFallback : null;

    static Source SourceOf(TreeInfo t, FStructFallback? handle, string path)
    {
        var src = new Source { Path = path };
        if (handle == null) return src;
        string source = TextOf(handle, "Source"), index = TextOf(handle, "Index");
        foreach (var s in t.Sources)
        {
            var dh = StructOf(s, "DataHandle");
            if (dh == null || TextOf(dh, "Source") != source || TextOf(dh, "Index") != index) continue;
            src.Kind = TextOf(s, "DataSource");
            src.Class = TextOf(s, "Struct");
            src.Name = TextOf(s, "Name");
            src.Global = source.Contains("Global", StringComparison.Ordinal);
            var state = StructOf(dh, "StateHandle");
            src.State = state != null ? IntOf(state, "Index") : -1;
            return src;
        }
        return src;
    }

    /// <summary>The bound settings of a node: the setting's name -> where its value comes from.</summary>
    static Dictionary<string, Source> BindingsOf(TreeInfo t, FStructFallback node)
    {
        var result = new Dictionary<string, Source>(StringComparer.Ordinal);
        int batch = IndexOf(node, "BindingsBatch");
        if (batch < 0 || batch >= t.Batches.Length) return result;
        int begin = IndexOf(t.Batches[batch], "BindingsBegin"), end = IndexOf(t.Batches[batch], "BindingsEnd");
        for (int i = begin; i >= 0 && i < end && i < t.Paths.Length; i++)
        {
            var p = t.Paths[i];
            string target = PathText(StructOf(p, "TargetPropertyPath"));
            if (target.Length > 0) result[target] = SourceOf(t, StructOf(p, "SourceDataHandle"), PathText(StructOf(p, "SourcePropertyPath")));
        }
        return result;
    }

    static double? NumberOf(object? v) => v switch
    {
        double d => d,
        float f => f,
        int i => i,
        long l => l,
        uint u => u,
        ulong ul => ul,
        short s => s,
        ushort us => us,
        byte b => b,
        sbyte sb => sb,
        _ => null
    };

    /// <summary>A setting's number from the node's instance data (an object or a struct).</summary>
    static double? InstanceNumber(UObject? obj, FStructFallback? str, string name)
    {
        if (obj != null) return NumberOf(obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue);
        if (str != null) return NumberOf(str.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue);
        return null;
    }

    static bool? InstanceBool(UObject? obj, FStructFallback? str, string name)
    {
        object? v = obj != null ? obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue : str?.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        return v is bool b ? b : null;
    }

    static bool HasInstanceProperty(UObject? obj, FStructFallback? str, string name) => obj != null ? obj.Properties.Any(p => p.Name.Text == name) : str != null && str.Properties.Any(p => p.Name.Text == name);

    /// <summary>The node's instance data: an object (a Blueprint or native condition class) or a struct (an engine condition).</summary>
    static void InstanceData(FStructFallback node, FStructFallback[] instances, out UObject? obj, out FStructFallback? str)
    {
        obj = null;
        str = null;
        if (!node.TryGetValue(out FStructFallback at, "InstanceTemplateIndex")) return;
        int i = IntOf(at, "Value");
        if (i < 0 || i >= instances.Length || instances[i] == null) return;
        if (instances[i].TryGetValue(out FPackageIndex o, "InstanceObject") && o != null && !o.IsNull) obj = Lv.Resolve(o);
        else str = instances[i];
    }

    /// <summary>A setting as a number: the value of the parameter it is bound to, or its own; null when it is bound to anything else or the parameter has no value.</summary>
    static double? Setting(UObject? obj, FStructFallback? str, Dictionary<string, Source> binds, Dictionary<string, object> prm, string name)
    {
        if (binds.TryGetValue(name, out var src))
        {
            if (src.Kind != "Parameter" || !prm.TryGetValue(src.Path, out var v)) return null;
            return NumberOf(v);
        }
        return InstanceNumber(obj, str, name);
    }

    static bool? SettingBool(UObject? obj, FStructFallback? str, Dictionary<string, Source> binds, Dictionary<string, object> prm, string name)
    {
        if (binds.TryGetValue(name, out var src))
        {
            if (src.Kind != "Parameter" || !prm.TryGetValue(src.Path, out var v)) return null;
            return v is bool b ? b : NumberOf(v) is { } n ? n != 0 : null;
        }
        return InstanceBool(obj, str, name);
    }

    static bool IsSelf(Source s) => s.Kind == "Context" && s.Name == "Actor" && s.Path.Length == 0;
    static bool IsPrimaryTarget(Source s) => s.Kind == "Evaluator" && (s.Class == "AoCSTE_UpdateMostHated" || s.Class == "STE_UpdateMostHated_C") && s.Path == "PrimaryTarget";   // (both name the most hated character)

    static string Escape(string s)
    {
        var sb = new System.Text.StringBuilder();
        foreach (char c in s)
        {
            if (char.IsAsciiLetterOrDigit(c) || c == '_' || c == '.' || c == '-') sb.Append(c);
            else sb.Append('%').Append(((int)c).ToString("X2", CultureInfo.InvariantCulture));
        }
        return sb.ToString();
    }

    static string Num(double v) => v.ToString("R", CultureInfo.InvariantCulture);

    static Atom Constant(bool value) => new Atom { Text = value ? "T" : "F", Constant = value ? "T" : "F" };
    static Atom Unsaid(string why) => new Atom { Text = "?" + Escape(why), Unknown = true };

    static string OpOf(string generic) => generic switch
    {
        "Equal" => "eq",
        "NotEqual" => "ne",
        "Less" => "lt",
        "LessOrEqual" => "le",
        "Greater" => "gt",
        "GreaterOrEqual" => "ge",
        _ => ""
    };

    static string Negate(string op) => op switch { "eq" => "ne", "ne" => "eq", "lt" => "ge", "ge" => "lt", "le" => "gt", "gt" => "le", _ => op };

    static bool Compare(string op, double a, double b) => op switch { "eq" => Math.Abs(a - b) < 1e-9, "ne" => Math.Abs(a - b) >= 1e-9, "lt" => a < b, "le" => a <= b, "gt" => a > b, "ge" => a >= b, _ => false };

    /// <summary>One condition node of a state's enter conditions, said as an atom.</summary>
    static Atom Encode(TreeInfo t, FStructFallback node, HashSet<int> path, Dictionary<string, object> prm)
    {
        string mode = TextOf(node, "EvaluationMode");
        if (mode == "ForcedTrue") return Constant(true);
        if (mode == "ForcedFalse") return Constant(false);
        string cls = ClassOf(node, "ConditionClass");
        InstanceData(node, t.Shared, out var obj, out var str);
        var binds = BindingsOf(t, node);
        bool Inv(string name) => (SettingBool(obj, str, binds, prm, name) ?? false);
        bool Targeted(string a, string b, bool ordered)
        {
            if (!binds.TryGetValue(a, out var sa) || !binds.TryGetValue(b, out var sb)) return false;
            return (IsSelf(sa) && IsPrimaryTarget(sb)) || (!ordered && IsPrimaryTarget(sa) && IsSelf(sb));
        }
        switch (cls)
        {
            case "AoCSTC_DistanceCheck":
            {
                var d = Setting(obj, str, binds, prm, "Distance");
                if (d == null) return Unsaid(cls + "(distance)");
                if (!Targeted("Agent", "Target", false)) return Unsaid(cls + "(other target)");
                bool inverse = Inv("bInverse");
                return new Atom { Text = (inverse ? "out(" : "in(") + Num(d.Value) + ")", Kind = "dist", Value = d.Value, Inverse = inverse };
            }
            case "AoCSTC_AngleCheck":
            {
                var a = Setting(obj, str, binds, prm, "Angle");
                if (a == null) return Unsaid(cls + "(angle)");
                if (!Targeted("Agent", "Target", true) || Inv("bInverse")) return Unsaid(cls + (Inv("bInverse") ? "(inverse)" : "(other target)"));
                return new Atom { Text = "ang(" + Num(a.Value) + ")", Kind = "ang", Value = a.Value };
            }
            case "AoCSTC_CooldownCheck":
            {
                string name = obj != null ? NameOf(obj, "TaskIdentifier") : "";
                if (name.Length == 0 || Inv("bInverse")) return Unsaid(cls + (name.Length == 0 ? "(no name)" : "(inverse)"));
                return new Atom { Text = "cd(" + Escape(name) + ")", Kind = "cd", Name = name };
            }
            case "STC_Distance_C":
            {
                var d = Setting(obj, str, binds, prm, "Distance");
                if (d == null) return Unsaid(cls + "(distance)");
                if (!Targeted("Agent", "Target", false)) return Unsaid(cls + "(other target)");
                return new Atom { Text = (Inv("Inverse") ? "cout(" : "cin(") + Num(d.Value) + ")" };
            }
            case "STC_Angle_C":
            {
                var a = Setting(obj, str, binds, prm, "Angle");
                if (a == null) return Unsaid(cls + "(angle)");
                if (!Targeted("Agent", "Target", true) || Inv("Inverse") || Inv("Directional")) return Unsaid(cls + (Inv("Directional") ? "(directional)" : Inv("Inverse") ? "(inverse)" : "(other target)"));
                return new Atom { Text = "ang(" + Num(a.Value) + ")", Kind = "ang", Value = a.Value };
            }
            case "AoCSTC_HealthPercentageCheck":
            case "STC_HealthPercentage_C":
            {
                var thr = Setting(obj, str, binds, prm, "Threshold");
                if (thr == null) return Unsaid(cls + "(threshold)");
                if (!binds.TryGetValue("Target", out var who) || !IsSelf(who)) return Unsaid(cls + "(other creature)");
                bool inverse = Inv(cls == "STC_HealthPercentage_C" ? "Inverse" : "bInverse");
                return new Atom { Text = (inverse ? "nhp(" : "hp(") + Num(thr.Value) + ")" };
            }
            case "STC_Random_C":
            {
                var p = Setting(obj, str, binds, prm, "Probability");
                return p == null ? Unsaid(cls + "(probability)") : new Atom { Text = "rand(" + Num(p.Value) + ")" };
            }
            case "STC_HasLineOfSight_C":
            {
                if (!binds.TryGetValue("Agent", out var agent) || !IsSelf(agent) || !binds.TryGetValue("TargetActor", out var target) || !IsPrimaryTarget(target)) return Unsaid(cls + "(other target)");
                return new Atom { Text = Inv("Inverse") ? "nlos" : "los" };
            }
        }
        if (cls.Length > 0) return Unsaid(cls);
        // an engine condition: it is told apart by the settings of its instance data
        if (str == null) return Unsaid("struct:" + TextOf(node, "Name"));
        bool invert = node.TryGetValue(out bool bInvert, "bInvert") && bInvert;
        if (HasInstanceProperty(null, str, "bLeft"))
        {
            var left = SettingBool(null, str, binds, prm, "bLeft");
            var right = SettingBool(null, str, binds, prm, "bRight");
            if (left == null || right == null) return Unsaid("Bool Compare(" + (binds.TryGetValue("bLeft", out var s0) ? s0.ToString() : "") + ")");
            return Constant((left.Value == right.Value) != invert);
        }
        if (HasInstanceProperty(null, str, "Threshold") && !HasInstanceProperty(null, str, "Left"))
        {
            var thr = Setting(null, str, binds, prm, "Threshold");
            return thr == null ? Unsaid("Random(threshold)") : new Atom { Text = "rand(" + Num(thr.Value) + ")" };
        }
        if (HasInstanceProperty(null, str, "Left") && HasInstanceProperty(null, str, "Right"))
        {
            string op = OpOf(TextOf(node, "Operator"));
            if (op.Length == 0) return Unsaid("Compare(operator)");
            if (invert) op = Negate(op);
            var right = Setting(null, str, binds, prm, "Right");
            if (right == null) return Unsaid("Compare(right:" + (binds.TryGetValue("Right", out var s1) ? s1.ToString() : "") + ")");
            if (binds.TryGetValue("Left", out var l) && l.Kind == "Task" && !l.Global && l.Name == "Combat Timer" && l.Path == "TaskElapsedTime" && path.Contains(l.State))
                return new Atom { Text = "fc(" + op + "," + Num(right.Value) + ")" };
            var leftValue = Setting(null, str, binds, prm, "Left");
            if (leftValue == null) return Unsaid("Compare(left:" + (binds.TryGetValue("Left", out var s2) ? s2.ToString() : "") + ")");
            return Constant(Compare(op, leftValue.Value, right.Value));
        }
        return Unsaid("struct:" + TextOf(node, "Name"));
    }

    /// <summary>The names of the states on a task's way down, root first.</summary>
    static bool InFight(TreeInfo t, List<int> path) => path.Any(p => string.Equals(TextOf(t.States[p], "Name"), "Combat", StringComparison.OrdinalIgnoreCase));
}
