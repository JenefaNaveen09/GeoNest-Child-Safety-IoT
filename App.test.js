import { render, screen } from "@testing-library/react";
import { onAuthStateChanged } from "firebase/auth";
import App from "./App";

jest.mock("firebase/auth", () => ({
  onAuthStateChanged: jest.fn(),
  signInWithEmailAndPassword: jest.fn(),
  signOut: jest.fn(),
}));
jest.mock("./firebase/config", () => ({ auth: {} }));
jest.mock("./components/Dashboard", () => () => null);

test("shows the sign-in form to a signed-out visitor", async () => {
  onAuthStateChanged.mockImplementation((auth, callback) => {
    callback(null);
    return () => {};
  });

  render(<App />);
  expect(await screen.findByRole("button", { name: "Sign in" })).toBeInTheDocument();
});
