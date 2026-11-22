package android.content;
/** Host stub: files dir from the system property "test.filesDir". */
public class Context {
    public Context getApplicationContext() { return this; }
    public java.io.File getFilesDir() { return new java.io.File(System.getProperty("test.filesDir")); }
}
