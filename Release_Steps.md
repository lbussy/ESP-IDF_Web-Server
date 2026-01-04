# ESP-IDF Component Release Checklist

Component: **http_server**  
Namespace: **lbussy**  
Repository type: **Single-component repository**

This checklist describes the exact steps to update and publish a new version of
an ESP-IDF component to the ESP Component Registry.

---

## 1. Prepare the release

- Ensure your working tree is clean
- All intended changes are committed
- `idf_component.yml` exists at the repository root

---

## 2. Bump the component version

Edit `idf_component.yml` and increment the version number.

```yaml
version: "X.Y.Z"
```

The version **must be new**. Published versions cannot be overwritten.

Commit the change:

```bash
git add idf_component.yml
git commit -m "Release X.Y.Z"
```

---

## 3. Verify build locally

Build an example project or a minimal test project that uses the component:

```bash
idf.py build
```

Fix all errors before continuing.

---

## 4. Authenticate to the ESP Component Registry

Generate an API token in the ESP registry UI and export it:

```bash
export IDF_COMPONENT_API_TOKEN="your_token_here"
```

You only need to do this once per environment or session.

---

## 5. Publish the component

From the **repository root**, run:

```bash
compote component upload --namespace lbussy --name http_server
```

This uploads the component version defined in `idf_component.yml`.

Notes:

- The upload is **immutable**
- If a mistake is found, you must bump the version and upload again

---

## 6. Verify publication

Confirm the new version appears in the registry:

- Search for `lbussy/http_server` in the ESP Component Registry UI

Test consuming the release:

```bash
idf.py add-dependency "lbussy/http_server==X.Y.Z"
```

---

## 7. Tag the release in Git (recommended)

Although not required by the registry, tagging is good practice:

```bash
git tag -a vX.Y.Z -m "Release vX.Y.Z"
git push origin vX.Y.Z
```

---

## Summary

- Update `idf_component.yml`
- Commit changes
- Build and test
- Upload using `compote component upload`
- Bump version for every fix

This checklist can be reused for every future release.
