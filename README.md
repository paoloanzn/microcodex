<p align="center"><strong>MicroCodex (uCodex)</strong> is a minimal coding agent that runs locally on your computer.</p>

<p align="center"><a href="https://github.com/paoloanzn/microcodex/actions/workflows/ci.yml"><img src="https://github.com/paoloanzn/microcodex/actions/workflows/ci.yml/badge.svg" alt="CI status"></a></p>

## Tests

Run the black-box CLI, API-streaming, and tool-loop suite on macOS or Linux with:

```sh
make test
```

The suite uses fake credentials and a loopback API fixture; it does not contact
the production service. See [`tests/TESTS.md`](tests/TESTS.md) for the harness
and test-case format.

## Known Bugs

- "Your input exceeds the context window of this model. Please adjust your input and try again."
