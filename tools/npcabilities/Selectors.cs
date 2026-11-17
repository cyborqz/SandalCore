// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.UObject;

namespace NpcAbilities;

/// <summary>
/// The utility-score target selector, `STC_UtilityScoreTargetSelector_C`: the condition a state puts on its task to score the actors its tree knows and take the best.  It has a
/// candidate expression (the characters the creature hates, the actors it perceives, the NPCs it spawned), a list of consideration expressions (each scored with one candidate as
/// GetTarget), a scoring scheme (the product, the highest, the average) and a blackboard variable the winner goes into.  This reads the part of those expressions the world can
/// keep into one atom, `pick(SET,SCHEME,USE,FLAGS,FILTER,CONSIDERATION...)`, every expression in postfix (the operands first, the operator last, ';' between).  A form it does
/// not know is said as a reason and the task stays held.  (The terms are in Conditions.cs and combat/npc_ability_data.h.)
/// </summary>
static class Selectors
{
    /// <summary>What the selector says, as the reader knows it; `Why` names the form it does not know (empty: all of it).</summary>
    public sealed class Rule
    {
        public string Set = "hated", Scheme = "product";
        public List<string> Flags = new();
        public string Filter = "";
        public List<string> Considerations = new();
        public string Bb = "";
        public bool Clear = true;
        public string Why = "";
    }

    /// <summary>The selector's atom, with USE "g" until the row's task says whether it reads the selector's blackboard (see Conditions.FixPickUses).</summary>
    public static string Atom(Rule r)
    {
        var flags = new List<string>(r.Flags);
        if (!r.Clear) flags.Add("!c");
        if (r.Bb.Length > 0) flags.Add("bb:" + Uri.EscapeDataString(r.Bb));
        var args = new List<string> { r.Set, r.Scheme, "g", string.Join(";", flags), r.Filter };
        args.AddRange(r.Considerations);
        return "pick(" + string.Join(",", args) + ")";
    }

    /// <summary>Reads a `STC_UtilityScoreTargetSelector_C` instance; null when its settings cannot be read at all.</summary>
    public static Rule? Read(UObject obj, FStructFallback? str)
    {
        var rule = new Rule { Clear = Bool(obj, str, "Clear BB Var at Failure") ?? true };
        string context = Expression(obj, str, "Consideration Context");
        if (context.Length == 0) return null;
        rule.Bb = Text(obj, str, "BB Storage");
        // the scheme: the class default (product) when the instance does not name one
        string scheme = Text(obj, str, "Scoring Scheme");
        rule.Scheme = scheme.EndsWith("NewEnumerator1", StringComparison.Ordinal) ? "highest" : scheme.EndsWith("NewEnumerator2", StringComparison.Ordinal) ? "average" : "product";
        rule.Set = CandidateSet(context, rule);
        if (rule.Why.Length > 0) return rule;
        // the considerations: each one expression, scored with one candidate as GetTarget
        var list = obj.Properties.FirstOrDefault(p => p.Name.Text == "Considerations")?.Tag?.GenericValue as UScriptArray;
        if (list != null)
        {
            foreach (var item in list.Properties)
            {
                string postfix = Expression(ExpressionText(item.GenericValue), out string why);
                if (why.Length > 0)
                {
                    rule.Why = why;
                    return rule;
                }
                rule.Considerations.Add(postfix);
            }
        }
        return rule;
    }

    /// <summary>A setting's text: a name, a string, or an enumeration's value.</summary>
    static string Text(UObject obj, FStructFallback? str, string name)
    {
        object? v = obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue ?? str?.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        return v switch
        {
            FName n => n.Text,
            string s => s,
            byte b => b.ToString(CultureInfo.InvariantCulture),
            int i => i.ToString(CultureInfo.InvariantCulture),
            long l => l.ToString(CultureInfo.InvariantCulture),
            _ => ""
        };
    }

    static bool? Bool(UObject obj, FStructFallback? str, string name) =>
        (obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue ?? str?.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue) is bool b ? b : null;

    static string Expression(UObject obj, FStructFallback? str, string name) =>
        ExpressionText(obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue ?? str?.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue);

    static string ExpressionText(object? v)
    {
        if (v is string s) return s;
        if (v is FScriptStruct ss && ss.StructType is FStructFallback f && f.TryGetValue(out string text, "Expression")) return text;
        return "";
    }

    // ---- the candidate set and its filter ----------------------------------------------------------------------------------------------------------

    // (the designer's line breaks inside an expression are kept: a comparison written on a line of its own is joined by Expression)
    static string Space(string text) => Regex.Replace(text, "[ \t]+", " ").Trim();
    static string Flat(string text) => Regex.Replace(text.Replace("\r", " ").Replace("\n", " "), "\\s+", " ").Trim();

    /// <summary>Whom the selector scores, with the filter each candidate must pass.  The set and the filter are parts of the pick atom.</summary>
    static string CandidateSet(string text, Rule rule)
    {
        string t = Space(text);
        // an ArrayCrop takes the first N of the set; a health sort orders it (the world crops its nearest, or its most hurt, candidates)
        var crop = Regex.Match(t, @"^ArrayCrop\(\s*(.*?)\s*,\s*(\d+)\s*\)$", RegexOptions.Singleline);
        if (crop.Success)
        {
            rule.Flags.Add("c" + crop.Groups[2].Value);
            t = crop.Groups[1].Value;
        }
        // the filter comes after a '#'; the set expression is in front of it (a SortByHealthPercentage wraps only the set)
        string filterText = "";
        int hash = t.IndexOf('#');
        if (hash >= 0)
        {
            filterText = t.Substring(hash + 1).Trim();
            if (filterText.StartsWith("{", StringComparison.Ordinal) && filterText.EndsWith("}", StringComparison.Ordinal)) filterText = filterText.Substring(1, filterText.Length - 2).Trim();
            t = t.Substring(0, hash).Trim();
        }
        var sorted = Regex.Match(t, @"^SortByHealthPercentage\(\s*(.*?)\s*\)$", RegexOptions.Singleline);
        if (sorted.Success)
        {
            rule.Flags.Add("s");
            t = sorted.Groups[1].Value;
        }
        var m = Regex.Match(t, @"^(GetHatedCharacters|GetPerceivedActors|GetSpawnedNpcsFromInstigator)\s*(?:\([^#]*\))?$", RegexOptions.Singleline);
        if (!m.Success)
        {
            rule.Why = "the selector's candidates (" + Short(Flat(text)) + ")";
            return "hated";
        }
        string function = m.Groups[1].Value;
        rule.Set = function == "GetHatedCharacters" ? "hated" : function == "GetPerceivedActors" ? "perceived" : "spawned";
        if (rule.Set == "spawned")
        {
            rule.Why = "the selector names a creature's spawned NPCs, which the world does not track";
            return rule.Set;
        }
        if (filterText.Length > 0)
        {
            string postfix = Expression(filterText, out string why);
            if (why.Length > 0)
            {
                rule.Why = why;
                return rule.Set;
            }
            rule.Filter = postfix;
        }
        return rule.Set;
    }

    // ---- the expressions ---------------------------------------------------------------------------------------------------------------------------

    static readonly Regex NearbyArray = new(@"GetArraySize\(\s*GetPerceivedActors\(\)\s*#\s*\{\s*GetDistanceBetween\(\s*(?:@|GetTarget\(\))\s*,\s*GetSource\(\)\s*\)\s*<\s*([0-9.]+)\s*\}\s*\)",
                                            RegexOptions.Singleline | RegexOptions.CultureInvariant);

    /// <summary>A comparison the designer wrote on a line of its own: two of them in a row are both meant (the same band other trees write with `&amp;&amp;`).</summary>
    static readonly Regex LineJoin = new(@"(?<=[)\w.])\s*(?:\r\n|\r|\n)\s*(?=(?:Get|Is|Has|Rand|Select|Max|Min|Abs|Clamp|Lerp|DotProduct|@|\())", RegexOptions.CultureInvariant);

    /// <summary>An expression as the world's postfix terms; `why` names the form the reader does not know.</summary>
    public static string Expression(string text, out string why)
    {
        why = "";
        try
        {
            // the one nested form: the number of perceived actors within a distance (the world asks every living actor near the creature)
            text = NearbyArray.Replace(text, "Nearby($1)");
            text = LineJoin.Replace(text, " && ");
            var program = new Program(text);
            if (!program.Run(out Node? value) || value == null)
            {
                why = program.Why.Length > 0 ? program.Why : "the selector's expression (" + Short(text) + ")";
                return "";
            }
            var sb = new StringBuilder();
            Emit(value, sb);
            return sb.ToString().TrimEnd(';');
        }
        catch (FormatException e)
        {
            why = e.Message.Length > 0 ? e.Message : "the selector's expression (" + Short(text) + ")";
            return "";
        }
    }

    static string Short(string text) => text.Length <= 60 ? text : text.Substring(0, 57) + "...";

    // ---- a small parser of the designer expressions -----------------------------------------------------------------------------------------------

    sealed class Node
    {
        public string Kind = "";        // num, src, cand, fwd, loc, norm, relationship, true, false, string, guid, ident, call
        public double Num;
        public string Text = "";
        public List<Node> Args = new();
        public static Node Of(string kind) => new Node { Kind = kind };
        public static Node Number(double v) => new Node { Kind = "num", Num = v };
        public static Node Call(string name, params Node[] args) => new Node { Kind = "call", Text = name, Args = args.ToList() };
        public static Node Op(string op, params Node[] args) => new Node { Kind = "call", Text = op, Args = args.ToList() };
    }

    sealed class Program
    {
        enum T { Ident, Number, String, Guid, Symbol, End }
        readonly record struct Token(T Kind, string Text, double Num);

        readonly List<Token> _tokens;
        int _at;
        readonly Dictionary<string, Node> _env = new(StringComparer.Ordinal);
        public string Why = "";

        public Program(string text) => _tokens = Lex(text);

        static List<Token> Lex(string text)
        {
            var outList = new List<Token>();
            for (int i = 0; i < text.Length;)
            {
                char c = text[i];
                if (char.IsWhiteSpace(c)) { ++i; continue; }
                if (c == '$')
                {
                    // a record reference: $#<something>:<guid>$
                    int close = text.IndexOf('$', i + 1);
                    if (close < 0) throw new FormatException("a record reference with no end");
                    string inside = text.Substring(i + 1, close - i - 1);
                    int colon = inside.LastIndexOf(':');
                    string number = colon >= 0 ? inside.Substring(colon + 1) : inside;
                    // (a record's guid is a whole number of 63 bits: kept as its digits, a double would round it and write it as 6.06E+18)
                    if (number.Length == 0 || !number.All(char.IsAsciiDigit) || !long.TryParse(number, NumberStyles.None, CultureInfo.InvariantCulture, out long guid)) throw new FormatException("a record reference that is not a whole number");
                    outList.Add(new Token(T.Guid, number, guid));
                    i = close + 1;
                    continue;
                }
                if (c == '"')
                {
                    int close = text.IndexOf('"', i + 1);
                    if (close < 0) throw new FormatException("a string with no end");
                    outList.Add(new Token(T.String, text.Substring(i + 1, close - i - 1), 0));
                    i = close + 1;
                    continue;
                }
                if (char.IsDigit(c) || (c == '.' && i + 1 < text.Length && char.IsDigit(text[i + 1])))
                {
                    int start = i;
                    while (i < text.Length && (char.IsDigit(text[i]) || text[i] == '.')) ++i;
                    string num = text.Substring(start, i - start);
                    outList.Add(new Token(T.Number, num, double.Parse(num, CultureInfo.InvariantCulture)));
                    continue;
                }
                if (char.IsLetter(c) || c == '_')
                {
                    int start = i;
                    while (i < text.Length && (char.IsLetterOrDigit(text[i]) || text[i] == '_')) ++i;
                    outList.Add(new Token(T.Ident, text.Substring(start, i - start), 0));
                    continue;
                }
                if (i + 1 < text.Length)
                {
                    string two = text.Substring(i, 2);
                    if (two is "&&" or "||" or "==" or "!=" or "<=" or ">=" or "::")
                    {
                        outList.Add(new Token(T.Symbol, two, 0));
                        i += 2;
                        continue;
                    }
                }
                if ("+-*/<>=!(){};,.@".IndexOf(c) >= 0)
                {
                    outList.Add(new Token(T.Symbol, c.ToString(), 0));
                    ++i;
                    continue;
                }
                throw new FormatException("a character the reader does not know (" + c + ")");
            }
            outList.Add(new Token(T.End, "", 0));
            return outList;
        }

        Token Peek => _tokens[_at];
        bool Take(string text)
        {
            if (Peek.Kind == T.Symbol && Peek.Text == text) { ++_at; return true; }
            return false;
        }
        bool TakeIdent(string text)
        {
            if (Peek.Kind == T.Ident && Peek.Text == text) { ++_at; return true; }
            return false;
        }
        void Expect(string text)
        {
            if (!Take(text)) throw new FormatException("the selector's expression: expected \"" + text + "\" near \"" + Peek.Text + "\"");
        }

        /// <summary>Runs the statements and leaves the program's value (the last expression, or the last variable it sets).</summary>
        public bool Run(out Node? value)
        {
            value = null;
            while (Peek.Kind != T.End)
            {
                if (!Statement(ref value)) return false;
            }
            return value != null;
        }

        bool Statement(ref Node? value)
        {
            if (TakeIdent("var") || TakeIdent("const") || TakeIdent("set"))
            {
                string name = Peek.Kind == T.Ident ? Peek.Text : "";
                if (name.Length == 0) throw new FormatException("the selector's expression: a variable with no name");
                ++_at;
                Expect("=");
                Node e = Expr();
                if (!Take(";") && Peek.Kind != T.End && Peek.Text != "}") throw new FormatException("the selector's expression: a statement with no \";\"");
                _env[name] = e;
                value = e;
                return true;
            }
            if (TakeIdent("if"))
            {
                Expect("(");
                Node cond = Expr();
                Expect(")");
                var then = new Dictionary<string, Node>(_env, StringComparer.Ordinal);
                if (!Block(then)) return false;
                var otherwise = new Dictionary<string, Node>(_env, StringComparer.Ordinal);
                if (TakeIdent("else")) if (!Block(otherwise)) return false;
                foreach (string key in then.Keys.Union(otherwise.Keys))
                {
                    then.TryGetValue(key, out Node? a);
                    otherwise.TryGetValue(key, out Node? b);
                    _env[key] = a == null ? b! : b == null || Same(a, b) ? a : Node.Op("if", cond, a, b);
                }
                return true;
            }
            value = Expr();
            if (!Take(";") && Peek.Kind != T.End && Peek.Text != "}") throw new FormatException("the selector's expression: an expression with no \";\"");
            return true;
        }

        bool Block(Dictionary<string, Node> scope)
        {
            var before = new Dictionary<string, Node>(_env, StringComparer.Ordinal);
            bool braced = Take("{");
            while (Peek.Kind != T.End && (!braced || Peek.Text != "}"))
            {
                Node? ignored = null;
                if (!Statement(ref ignored)) return false;
            }
            if (braced) Expect("}");
            scope.Clear();
            foreach (var kv in _env) scope[kv.Key] = kv.Value;
            _env.Clear();
            foreach (var kv in before) _env[kv.Key] = kv.Value;
            return true;
        }

        static bool Same(Node a, Node b) => a.Kind == b.Kind && a.Text == b.Text && Math.Abs(a.Num - b.Num) < 1e-12 && a.Args.Count == b.Args.Count && a.Args.Zip(b.Args).All(p => Same(p.First, p.Second));

        Node Expr() => Or();
        Node Or()
        {
            Node left = And();
            while (Take("||")) left = Node.Op("or", left, And());
            return left;
        }
        Node And()
        {
            Node left = Equality();
            while (Take("&&")) left = Node.Op("and", left, Equality());
            return left;
        }
        Node Equality()
        {
            Node left = Relation();
            for (;;)
            {
                string op = Take("==") ? "eq" : Take("!=") ? "ne" : "";
                if (op.Length == 0) return left;
                left = Compare(op, left, Relation());
            }
        }
        Node Relation()
        {
            Node left = Add();
            for (;;)
            {
                string op = Take("<") ? "lt" : Take("<=") ? "le" : Take(">") ? "gt" : Take(">=") ? "ge" : "";
                if (op.Length == 0) return left;
                left = Node.Op(op, left, Add());
            }
        }
        Node Add()
        {
            Node left = Mul();
            for (;;)
            {
                if (Take("+")) left = Node.Op("+", left, Mul());
                else if (Take("-")) left = Node.Op("-", left, Mul());
                else return left;
            }
        }
        Node Mul()
        {
            Node left = Unary();
            for (;;)
            {
                if (Take("*")) left = Node.Op("*", left, Unary());
                else if (Take("/")) left = Node.Op("/", left, Unary());
                else return left;
            }
        }
        Node Unary()
        {
            if (Take("!")) return Node.Op("not", Unary());
            if (Take("-")) return Node.Op("neg", Unary());
            return Primary();
        }

        Node Primary()
        {
            if (Peek.Kind == T.Number)
            {
                double v = Peek.Num;
                ++_at;
                return Node.Number(v);
            }
            if (Peek.Kind == T.Guid) { Node g = new Node { Kind = "guid", Num = Peek.Num, Text = Peek.Text }; ++_at; return g; }
            if (Peek.Kind == T.String) { Node s = new Node { Kind = "string", Text = Peek.Text }; ++_at; return s; }
            if (Take("@")) return Node.Of("cand");
            if (Take("("))
            {
                Node e = Expr();
                Expect(")");
                return e;
            }
            if (Peek.Kind != T.Ident) throw new FormatException("the selector's expression: an operand the reader does not know near \"" + Peek.Text + "\"");
            string name = Peek.Text;
            ++_at;
            // an enumeration value: ECombatRelationship::Friendly
            if (Take("::"))
            {
                string member = Peek.Kind == T.Ident ? Peek.Text : "";
                if (member.Length == 0) throw new FormatException("the selector's expression: an enumeration value with no member");
                ++_at;
                return new Node { Kind = "ident", Text = name + "::" + member };
            }
            if (name == "true") return Node.Of("true");
            if (name == "false") return Node.Of("false");
            if (!Take("("))
            {
                if (_env.TryGetValue(name, out Node? variable)) return variable;
                if (name == "GetSelf" || name == "GetSource") return Node.Of("src");
                throw new FormatException("the selector's expression: a name the reader does not know (" + name + ")");
            }
            var args = new List<Node>();
            if (!Take(")"))
            {
                do { args.Add(Expr()); } while (Take(","));
                Expect(")");
            }
            return Call(name, args);
        }

        Node Call(string name, List<Node> args)
        {
            switch (name)
            {
                case "GetSelf":
                case "GetSource":
                    return Node.Of("src");
                case "GetTarget":
                case "GetIteratingContext":
                    return Node.Of("cand");
                case "GetActorForward":
                    return Node.Of("fwd");
                case "GetLocation":
                    return Node.Of("loc");
                case "Normalize":
                    return Node.Of("norm");
                case "GetDistanceBetween":
                    if (args.Count == 2 && ActorsAreSourceAndCandidate(args[0], args[1])) return Node.Of("dist");
                    Why = "a distance between actors the reader cannot follow";
                    break;
                case "GetHealthPercentage":
                    if (args.Count == 1 && args[0].Kind == "cand") return Node.Of("health");
                    Why = "a health percentage of an actor other than the candidate";
                    break;
                case "GetAngleBetween":
                    if (args.Count == 2 && args[0].Kind == "src" && args[1].Kind == "cand") return Node.Of("angle");
                    if (args.Count == 2 && args[0].Kind == "cand" && args[1].Kind == "src") return Node.Of("angle_of_candidate");
                    Why = "an angle between actors the reader cannot follow";
                    break;
                case "GetEffectCount":
                    return Node.Call("effectcount", args.ToArray());
                case "GetLineOfSight":
                case "GetLineofSight":
                    return Node.Of("los");
                case "IsFriendly":
                    return Node.Of("friendly");
                case "GetCombatRelationship":
                    return Node.Of("relationship");
                case "HasAlignment":
                    return Node.Call("alignment", args.ToArray());
                case "GetNpcEliteRank":
                case "GetNPCEliteRank":
                    return Node.Call("rank", args.ToArray());
                case "GetMostHatedCharacter":
                    return Node.Of("most");
                case "GetBlackboardGuid":
                    return Node.Call("blackboard", args.ToArray());
                case "GetBlackboardBool":
                    Why = "the selector reads the target's own blackboard (GetBlackboardBool), which the world does not keep";
                    break;
                case "RandDouble":
                    return Node.Of("random");
                case "RandDoubleRange":
                    return Node.Call("randrange", args.ToArray());
                case "SelectFloat":
                    return Node.Call("select", args.ToArray());
                case "Max":
                case "Min":
                case "Abs":
                case "Clamp":
                case "Lerp":
                    return Node.Call(name.ToLowerInvariant(), args.ToArray());
                case "DotProduct":
                    if (args.Count == 2 && args[0].Kind == "fwd" && args[1].Kind == "norm") return Node.Of("dot");
                    Why = "a dot product of a form the reader does not know";
                    break;
                case "IsDead":
                    return Node.Of("dead");
                case "Nearby":
                    return Node.Call("nearby", args.ToArray());
                case "GetArraySize":
                    Why = "the selector asks for the size of an actor array the reader does not know (GetArraySize)";
                    break;
                default:
                    Why = "the selector's expression form (" + name + ")";
                    break;
            }
            throw new FormatException(Why);
        }

        static bool ActorsAreSourceAndCandidate(Node a, Node b) =>
            (a.Kind == "src" && b.Kind == "cand") || (a.Kind == "cand" && b.Kind == "src");

        /// <summary>An equality that is really a property of one actor (the source, the candidate, the relationship between them).</summary>
        Node Compare(string op, Node left, Node right)
        {
            if (left.Kind == "relationship") return Relationship(op, right);
            if (right.Kind == "relationship") return Relationship(op, left);
            if (left.Kind == "src" && right.Kind == "src") return Node.Number(1);
            if (left.Kind == "cand" && right.Kind == "cand") return Node.Number(1);
            // (the candidate against the most hated character: the hate list's own target; this has to come before the rule below, which reads any actor against an actor as "is the creature")
            if ((left.Kind == "cand" && right.Kind == "most") || (left.Kind == "most" && right.Kind == "cand")) return op == "eq" ? Node.Of("most") : Node.Op("not", Node.Of("most"));
            if (left.Kind == "most" || right.Kind == "most") throw new FormatException("the selector's expression: the most hated character compared with something other than the candidate");
            if (left.Kind == "cand" && right.Kind == "call" && right.Text == "blackboard") return op == "eq" ? right : Node.Op("not", right);
            if (right.Kind == "cand" && left.Kind == "call" && left.Text == "blackboard") return op == "eq" ? left : Node.Op("not", left);
            if (left.Kind == "src" || right.Kind == "src" || left.Kind == "cand" || right.Kind == "cand") return op == "eq" ? Node.Of("self") : Node.Op("not", Node.Of("self"));
            if (left.Kind == "ident") left = Resolve(left);
            if (right.Kind == "ident") right = Resolve(right);
            if (op == "eq" && left.Kind == "num" && right.Kind == "num") return Node.Number(Math.Abs(left.Num - right.Num) < 1e-9 ? 1 : 0);
            return Node.Op(op, left, right);
        }

        Node Relationship(string op, Node other)
        {
            string text = other.Kind == "ident" ? other.Text : other.Kind == "relationship" ? "ECombatRelationship::Friendly" : "";
            bool friendly = text.EndsWith("::Friendly", StringComparison.Ordinal);
            bool hostile = text.EndsWith("::Hostile", StringComparison.Ordinal);
            if (!friendly && !hostile) throw new FormatException("the selector's expression: a relationship the reader does not know");
            Node value = friendly ? Node.Of("friendly") : Node.Op("not", Node.Of("friendly"));
            return op == "eq" ? value : Node.Op("not", value);
        }

        Node Resolve(Node ident) => _env.TryGetValue(ident.Text, out Node? v) ? v : ident;
    }

    // ---- the program's value as the world's postfix terms ------------------------------------------------------------------------------------------

    static bool Emit(Node n, StringBuilder sb)
    {
        static string N(double v) => v.ToString("R", CultureInfo.InvariantCulture);
        switch (n.Kind)
        {
            case "num": sb.Append(N(n.Num)).Append(';'); return true;
            case "true": sb.Append("1;"); return true;
            case "false": sb.Append("0;"); return true;
            case "random": sb.Append("r;"); return true;
            case "dist": sb.Append("d;"); return true;
            case "health": sb.Append("h;"); return true;
            case "los": sb.Append("los;"); return true;
            case "angle": sb.Append("ang;"); return true;
            case "angle_of_candidate": sb.Append("angc;"); return true;
            case "friendly": sb.Append("friendly;"); return true;
            case "most": sb.Append("most;"); return true;
            case "self": sb.Append("self;"); return true;
            case "dead": sb.Append("dead;"); return true;
            case "dot": sb.Append("face;"); return true;
            case "call":
                if (n.Text is "effectcount" or "alignment" or "blackboard") return EmitActorValue(n, sb);
                if (n.Text == "rank")
                {
                    sb.Append(n.Args.Count == 1 && n.Args[0].Kind == "src" ? "srank;" : "rank;");
                    return true;
                }
                int expected = n.Text switch
                {
                    "not" or "neg" or "abs" or "nearby" => 1,
                    "clamp" or "lerp" or "select" or "if" => 3,
                    _ => 2
                };
                if (n.Args.Count != expected) throw new FormatException("the selector's expression: " + n.Text + " with " + n.Args.Count + " operand(s) instead of " + expected);
                foreach (var a in n.Args) if (!Emit(a, sb)) return false;
                sb.Append(n.Text switch
                {
                    "+" => "+;",
                    "-" => "-;",
                    "*" => "*;",
                    "/" => "/;",
                    "lt" => "lt;",
                    "le" => "le;",
                    "gt" => "gt;",
                    "ge" => "ge;",
                    "eq" => "eq;",
                    "ne" => "ne;",
                    "and" => "and;",
                    "or" => "or;",
                    "not" => "not;",
                    "neg" => "neg;",
                    "abs" => "abs;",
                    "max" => "max;",
                    "min" => "min;",
                    "clamp" => "clamp;",
                    "lerp" => "lerp;",
                    "select" => "if;",
                    "if" => "if;",
                    "randrange" => "rr;",
                    "nearby" => "near;",
                    _ => throw new FormatException("the selector's expression: a function the reader does not know (" + n.Text + ")")
                });
                return true;
            default:
                throw new FormatException("the selector's expression: a form the reader does not know (" + n.Kind + ")");
        }
    }

    /// <summary>The terms whose first argument names the actor they ask about: an effect count, an alignment, the source's blackboard.</summary>
    static bool EmitActorValue(Node n, StringBuilder sb)
    {
        static string N(double v) => v.ToString("R", CultureInfo.InvariantCulture);
        if (n.Args.Count < 2) throw new FormatException("the selector's expression: an actor term with no actor or value");
        Node actor = n.Args[0];
        switch (n.Text)
        {
            case "effectcount":
                if (n.Args[1].Kind != "guid") throw new FormatException("the selector's expression: an effect count the reader cannot follow");
                if (actor.Kind == "cand") sb.Append("eff:").Append(n.Args[1].Text).Append(';');
                else if (actor.Kind == "src") sb.Append("seff:").Append(n.Args[1].Text).Append(';');
                else throw new FormatException("the selector's expression: an effect count of an actor other than the candidate");
                return true;
            case "alignment":
                if (actor.Kind != "cand" || n.Args[1].Kind != "guid") throw new FormatException("the selector's expression: an alignment check the reader cannot follow");
                sb.Append("align:").Append(n.Args[1].Text).Append(';');
                return true;
            case "blackboard":
                if (actor.Kind != "src" || n.Args[1].Kind != "string") throw new FormatException("the selector's expression: a blackboard read the reader cannot follow");
                sb.Append("bb:").Append(Uri.EscapeDataString(n.Args[1].Text)).Append(';');
                return true;
        }
        return false;
    }
}

