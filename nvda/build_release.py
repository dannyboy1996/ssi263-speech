"""Build all three NVDA packages together, sharing native builds and chip checks."""
import build_blazie
import build_accent
import build_speakout


def main():
    builders = (build_blazie, build_accent, build_speakout)
    versions = {b.VERSION for b in builders}
    if len(versions) != 1:
        raise SystemExit("Release manifests disagree: %s" % versions)
    for builder in builders:
        builder.main()


if __name__ == "__main__":
    main()
