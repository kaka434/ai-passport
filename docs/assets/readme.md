<p align="right">
  <a href="readme.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# docs/assets

Fork-private material that supplements the root `README.md`: product photos,
architecture notes, design drafts and images that belong to this fork rather than
to the upstream baseline. Upstream keeps this directory empty except for
`.gitkeep`.

## Structure

- One subdirectory or one file prefix per application, named after the
  application in lowercase-kebab-case, e.g. `football-live-board-scores.jpg`.
- Binary material is stored here, never mixed into Markdown documentation.
- Any copyrighted or licensed material records its source and license in the README
  that references it.

## How to use it

- The root `README.md` of a `feature/*` branch links into this directory instead of
  copying large images next to the text.
- When entering the repository, read the root `README.md` together with the files
  here; the upstream overview stays at `docs/README.md`.
- Regenerating or replacing material is a normal change on the feature branch; keep
  the file names stable so links do not break.

## Boundary

- This directory exists only on this fork. Its content is fork-private and must never
  be proposed back to the upstream project.
- The upstream sync workflow only brings upstream `main` into this fork's `main`;
  keep fork-private material on `feature/*` branches so a sync cannot conflict with
  it.
- If a document turns out to be useful for any AI Passport user rather than only for
  this fork, move it out of here and propose it upstream as a documentation pull
  request.
