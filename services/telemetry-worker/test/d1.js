// A D1 stand-in over node:sqlite, enough for the Worker's prepare/bind/all/first/run calls.
import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";

export function fakeD1() {
  const db = new DatabaseSync(":memory:");
  db.exec(readFileSync(new URL("../schema.sql", import.meta.url), "utf8"));
  return {
    prepare(sql) {
      const stmt = db.prepare(sql);
      let args = [];
      const bound = {
        bind(...a) {
          args = a.map((v) => (v === undefined ? null : typeof v === "boolean" ? Number(v) : v));
          return bound;
        },
        async all() {
          return { results: stmt.all(...args).map((r) => ({ ...r })) };
        },
        async first() {
          const r = stmt.get(...args);
          return r ? { ...r } : null;
        },
        async run() {
          return stmt.run(...args);
        },
      };
      return bound;
    },
  };
}
