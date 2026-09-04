package space.osbt.bitsdk;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Minimal self-contained JSON parser and serializer for BITSDK.
 *
 * <p>Parse output types: LinkedHashMap (objects), ArrayList (arrays),
 * String, Long/Double (numbers), Boolean, null. Serialize accepts Map,
 * List, String, Number, Boolean and null. Non-ASCII characters are never
 * escaped (UTF-8 is preserved verbatim).
 */
public final class BitJson {

    private BitJson() {
    }

    // ------------------------------------------------------------------
    // Parsing
    // ------------------------------------------------------------------

    /** Parses a JSON document into Java objects. Throws IllegalArgumentException on invalid input. */
    public static Object parse(String text) {
        if (text == null) {
            throw new IllegalArgumentException("null JSON input");
        }
        Parser p = new Parser(text);
        p.skipWhitespace();
        Object value = p.parseValue();
        p.skipWhitespace();
        if (!p.atEnd()) {
            throw p.error("unexpected trailing characters");
        }
        return value;
    }

    private static final class Parser {
        private final String s;
        private int i;

        Parser(String s) {
            this.s = s;
        }

        boolean atEnd() {
            return i >= s.length();
        }

        IllegalArgumentException error(String message) {
            return new IllegalArgumentException(message + " at position " + i);
        }

        void skipWhitespace() {
            while (i < s.length()) {
                char c = s.charAt(i);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    i++;
                } else {
                    break;
                }
            }
        }

        private char peek() {
            if (atEnd()) {
                throw error("unexpected end of input");
            }
            return s.charAt(i);
        }

        private void expect(char c) {
            if (atEnd() || s.charAt(i) != c) {
                throw error("expected '" + c + "'");
            }
            i++;
        }

        Object parseValue() {
            if (atEnd()) {
                throw error("unexpected end of input");
            }
            switch (peek()) {
                case '{':
                    return parseObject();
                case '[':
                    return parseArray();
                case '"':
                    return parseString();
                case 't':
                    expectWord("true");
                    return Boolean.TRUE;
                case 'f':
                    expectWord("false");
                    return Boolean.FALSE;
                case 'n':
                    expectWord("null");
                    return null;
                default:
                    return parseNumber();
            }
        }

        private void expectWord(String word) {
            if (i + word.length() > s.length() || !s.startsWith(word, i)) {
                throw error("invalid literal");
            }
            i += word.length();
        }

        private Map<String, Object> parseObject() {
            expect('{');
            Map<String, Object> map = new LinkedHashMap<String, Object>();
            skipWhitespace();
            if (peek() == '}') {
                i++;
                return map;
            }
            while (true) {
                skipWhitespace();
                if (peek() != '"') {
                    throw error("expected object key");
                }
                String key = parseString();
                skipWhitespace();
                expect(':');
                skipWhitespace();
                map.put(key, parseValue());
                skipWhitespace();
                char c = peek();
                if (c == ',') {
                    i++;
                    continue;
                }
                if (c == '}') {
                    i++;
                    return map;
                }
                throw error("expected ',' or '}'");
            }
        }

        private List<Object> parseArray() {
            expect('[');
            List<Object> list = new ArrayList<Object>();
            skipWhitespace();
            if (peek() == ']') {
                i++;
                return list;
            }
            while (true) {
                skipWhitespace();
                list.add(parseValue());
                skipWhitespace();
                char c = peek();
                if (c == ',') {
                    i++;
                    continue;
                }
                if (c == ']') {
                    i++;
                    return list;
                }
                throw error("expected ',' or ']'");
            }
        }

        private String parseString() {
            expect('"');
            StringBuilder sb = new StringBuilder();
            while (true) {
                if (atEnd()) {
                    throw error("unterminated string");
                }
                char c = s.charAt(i++);
                if (c == '"') {
                    return sb.toString();
                }
                if (c == '\\') {
                    if (atEnd()) {
                        throw error("unterminated escape sequence");
                    }
                    char e = s.charAt(i++);
                    switch (e) {
                        case '"': sb.append('"'); break;
                        case '\\': sb.append('\\'); break;
                        case '/': sb.append('/'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case 'n': sb.append('\n'); break;
                        case 'r': sb.append('\r'); break;
                        case 't': sb.append('\t'); break;
                        case 'u':
                            if (i + 4 > s.length()) {
                                throw error("invalid \\u escape");
                            }
                            sb.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                            i += 4;
                            break;
                        default:
                            throw error("invalid escape '\\" + e + "'");
                    }
                } else {
                    sb.append(c);
                }
            }
        }

        private Object parseNumber() {
            int start = i;
            if (peek() == '-') {
                i++;
            }
            // integer part
            if (atEnd() || !isDigit(s.charAt(i))) {
                throw error("invalid number");
            }
            if (s.charAt(i) == '0') {
                i++;
            } else {
                while (i < s.length() && isDigit(s.charAt(i))) {
                    i++;
                }
            }
            // fraction
            if (i < s.length() && s.charAt(i) == '.') {
                i++;
                if (atEnd() || !isDigit(s.charAt(i))) {
                    throw error("invalid number fraction");
                }
                while (i < s.length() && isDigit(s.charAt(i))) {
                    i++;
                }
            }
            // exponent
            if (i < s.length() && (s.charAt(i) == 'e' || s.charAt(i) == 'E')) {
                i++;
                if (i < s.length() && (s.charAt(i) == '+' || s.charAt(i) == '-')) {
                    i++;
                }
                if (atEnd() || !isDigit(s.charAt(i))) {
                    throw error("invalid number exponent");
                }
                while (i < s.length() && isDigit(s.charAt(i))) {
                    i++;
                }
            }
            String text = s.substring(start, i);
            boolean isDecimal = text.indexOf('.') >= 0
                    || text.indexOf('e') >= 0 || text.indexOf('E') >= 0;
            if (isDecimal) {
                return Double.valueOf(text);
            }
            try {
                return Long.valueOf(text);
            } catch (NumberFormatException overflow) {
                return Double.valueOf(text);
            }
        }

        private boolean isDigit(char c) {
            return c >= '0' && c <= '9';
        }
    }

    // ------------------------------------------------------------------
    // Serialization
    // ------------------------------------------------------------------

    /** Serializes a Map/List/String/Number/Boolean/null tree into a JSON string. */
    public static String write(Object value) {
        StringBuilder sb = new StringBuilder();
        writeValue(value, sb);
        return sb.toString();
    }

    private static void writeValue(Object value, StringBuilder sb) {
        if (value == null) {
            sb.append("null");
        } else if (value instanceof String) {
            writeString((String) value, sb);
        } else if (value instanceof Boolean) {
            sb.append(((Boolean) value).booleanValue() ? "true" : "false");
        } else if (value instanceof Double || value instanceof Float) {
            double d = ((Number) value).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d)) {
                sb.append("null"); // JSON has no NaN/Infinity
            } else {
                sb.append(value.toString());
            }
        } else if (value instanceof Number) {
            sb.append(value.toString()); // Long, Integer, BigInteger, ...
        } else if (value instanceof Map<?, ?>) {
            sb.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> entry : ((Map<?, ?>) value).entrySet()) {
                if (!first) {
                    sb.append(',');
                }
                first = false;
                writeString(String.valueOf(entry.getKey()), sb);
                sb.append(':');
                writeValue(entry.getValue(), sb);
            }
            sb.append('}');
        } else if (value instanceof List<?>) {
            sb.append('[');
            boolean first = true;
            for (Object element : (List<?>) value) {
                if (!first) {
                    sb.append(',');
                }
                first = false;
                writeValue(element, sb);
            }
            sb.append(']');
        } else {
            throw new IllegalArgumentException(
                    "cannot serialize type: " + value.getClass().getName());
        }
    }

    private static void writeString(String s, StringBuilder sb) {
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\b': sb.append("\\b"); break;
                case '\f': sb.append("\\f"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c); // non-ASCII passes through unescaped
                    }
            }
        }
        sb.append('"');
    }
}
