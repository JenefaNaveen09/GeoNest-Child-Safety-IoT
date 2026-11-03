import { useEffect, useState } from "react";
import { LoadScript } from "@react-google-maps/api";
import { onAuthStateChanged, signInWithEmailAndPassword, signOut } from "firebase/auth";
import Dashboard from "./components/Dashboard";
import { auth } from "./firebase/config";

export default function App() {
  const [user, setUser] = useState(undefined);
  const [error, setError] = useState("");
  const [pending, setPending] = useState(false);

  useEffect(() => onAuthStateChanged(auth, setUser), []);

  async function signIn(event) {
    event.preventDefault();
    const fields = new FormData(event.currentTarget);
    setError("");
    setPending(true);
    try {
      await signInWithEmailAndPassword(auth, fields.get("email"), fields.get("password"));
    } catch {
      setError("Sign-in failed. Check your demo account and Firebase configuration.");
    } finally {
      setPending(false);
    }
  }

  if (user === undefined) return <p>Checking sign-in status…</p>;
  if (!user) {
    return (
      <main className="mx-auto max-w-sm p-6">
        <h1 className="mb-4 text-xl font-bold">GeoNest demo sign-in</h1>
        <form onSubmit={signIn} className="grid gap-3">
          <label>Email <input name="email" type="email" autoComplete="username" required /></label>
          <label>Password <input name="password" type="password" autoComplete="current-password" required /></label>
          <button type="submit" disabled={pending} className="rounded bg-blue-700 p-2 text-white">Sign in</button>
        </form>
        {error && <p role="alert">{error}</p>}
      </main>
    );
  }

  return (
    <>
      <button type="button" onClick={() => signOut(auth).catch(() => setError("Sign-out failed. Try again."))}
        className="fixed bottom-4 right-4 z-50 rounded bg-blue-700 p-2 text-white">
        Sign out
      </button>
      {error && <p role="alert">{error}</p>}
      <LoadScript googleMapsApiKey={process.env.REACT_APP_GOOGLE_MAPS_API_KEY} loadingElement={<p>Loading map…</p>}>
        <Dashboard />
      </LoadScript>
    </>
  );
}
