"""Opt-in diagnostics for the Mirage Python command line."""

from __future__ import annotations

import os
import sys
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from time import perf_counter
from typing import TYPE_CHECKING

import click

if TYPE_CHECKING:
    from loguru import Logger

ROOT = Path(__file__).resolve().parents[2]
LOG_PATH = ROOT / "build/logs/mirage-tools.log"
ROTATION = ["10 MB", "00:00"]
_ENABLED = {"1", "true", "yes", "on"}


@contextmanager
def _sinks() -> Iterator[Logger | None]:
    if os.environ.get("MIRAGE_ENABLE_LOGGING", "").strip().lower() not in _ENABLED:
        yield None
        return
    try:
        from loguru import logger
    except ImportError:
        click.echo("MIRAGE_ENABLE_LOGGING requires Loguru. Install the Mirage tooling dependencies.", err=True)
        yield None
        return

    # Loguru installs a console sink on import. Replace it with the CLI's stderr sink.
    try:
        logger.remove(0)
    except ValueError:
        pass

    def is_mirage_record(record) -> bool:
        return record["extra"].get("mirage_tool", False)

    handlers = [logger.add(sys.stderr, level="INFO", filter=is_mirage_record, diagnose=False, backtrace=False,
                           format="<level>{level}</level> {message}")]
    log = logger.bind(mirage_tool=True)
    try:
        try:
            handlers.append(logger.add(
                LOG_PATH, level="DEBUG", filter=is_mirage_record, encoding="utf-8", watch=True,
                rotation=ROTATION, retention="30 days", compression="gz", diagnose=False, backtrace=False,
                format="{time:YYYY-MM-DD HH:mm:ss.SSS Z} [{process.id}] {level} {message}",
            ))
        except OSError as exc:
            log.warning("Could not open Python tool log: {}", exc)
        yield log
    finally:
        for handler in reversed(handlers):
            logger.remove(handler)


def command_logger() -> Logger | None:
    """Get the active tool logger without importing or enabling Loguru."""
    context = click.get_current_context(silent=True)
    return context.meta.get("mirage_tool_logger") if context else None


class LoggedGroup(click.Group):
    """Record the command outcome without logging its arguments."""

    group_class = type

    def invoke(self, ctx: click.Context):
        if ctx.parent is not None:
            try:
                return super().invoke(ctx)
            finally:
                if command_logger() and "mirage_tool_command" not in ctx.meta:
                    ctx.meta["mirage_tool_command"] = self._command_name(ctx)

        with _sinks() as log:
            if log is None:
                return super().invoke(ctx)
            ctx.meta["mirage_tool_logger"] = log
            started = perf_counter()
            log.debug("{} started", ctx.command_path)
            try:
                result = super().invoke(ctx)
            except (click.exceptions.Exit, click.ClickException, SystemExit) as exc:
                exit_code = exc.code if isinstance(exc, SystemExit) else exc.exit_code
                self._finished(ctx, log, started, 0 if exit_code is None else exit_code)
                raise
            except (KeyboardInterrupt, click.Abort):
                log.warning("{} aborted", self._command_name(ctx))
                raise
            except Exception:
                log.opt(exception=True).error("{} failed", self._command_name(ctx))
                raise
            else:
                self._finished(ctx, log, started, 0)
                return result
            finally:
                ctx.meta.pop("mirage_tool_logger", None)
                ctx.meta.pop("mirage_tool_command", None)

    @staticmethod
    def _command_name(ctx: click.Context) -> str:
        return f"{ctx.command_path} {ctx.invoked_subcommand}" if ctx.invoked_subcommand else ctx.command_path

    @classmethod
    def _finished(cls, ctx: click.Context, log: Logger, started: float, code: object) -> None:
        name = ctx.meta.get("mirage_tool_command", cls._command_name(ctx))
        level = "INFO" if code == 0 else "WARNING"
        log.log(level, "{} exited with {} in {:.2f}s", name, code if isinstance(code, int) else 1,
                perf_counter() - started)
