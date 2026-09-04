package space.osbt.bitsdk;

import java.util.Map;

/**
 * Error thrown by {@link BitClient} for HTTP-level failures. Carries the HTTP
 * status and the parsed response body. {@link #getMessage()} returns the
 * server-provided error text: for {@code {"error":{...}}} the inner
 * {@code message}, for {@code {"error":"..."}} the string itself.
 */
public class BitException extends RuntimeException {

    private final int status;
    private final transient Object raw;

    public BitException(int status, String bodyText) {
        super(buildMessage(status, bodyText));
        this.status = status;
        this.raw = tryParse(bodyText);
    }

    public BitException(int status, String message, Object raw) {
        super(message);
        this.status = status;
        this.raw = raw;
    }

    /** HTTP status code of the failed response. */
    public int getStatus() {
        return status;
    }

    /** Parsed response body (Map/List/...), or null when the body was not JSON. */
    public Object getRaw() {
        return raw;
    }

    private static Object tryParse(String bodyText) {
        if (bodyText == null || bodyText.isEmpty()) {
            return null;
        }
        try {
            return BitJson.parse(bodyText);
        } catch (RuntimeException e) {
            return null;
        }
    }

    @SuppressWarnings("unchecked")
    static String buildMessage(int status, String bodyText) {
        Object raw = tryParse(bodyText);
        if (raw instanceof Map) {
            Object error = ((Map<String, Object>) raw).get("error");
            if (error instanceof Map) {
                Object message = ((Map<String, Object>) error).get("message");
                if (message instanceof String) {
                    return (String) message;
                }
            }
            if (error instanceof String) {
                return (String) error;
            }
        }
        return (bodyText == null || bodyText.isEmpty()) ? ("HTTP " + status) : bodyText;
    }
}
