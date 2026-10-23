"""Canonical content import pipeline: frontends -> IR -> validated cook -> mod package.

See docs/content-pipeline.md. Every imported model (prop, building, mech) passes
through content_pipeline.ir.Asset; frontends are thin adapters into it.
"""
