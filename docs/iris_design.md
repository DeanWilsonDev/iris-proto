# Iris UI Framework Design

## Overview

Iris is a JSX-style UI component framework for building in-game
interfaces in **Umbra Engine**.

Iris files combine structure and logic in a single `.iris` file,
similar to how JSX combines HTML-like markup with JavaScript. The
syntax has Nyx DNA but parses differently to accommodate declarative
UI structure alongside imperative logic.

The goal of Iris is to make game UI development feel like modern web
development — compositional, plain text, version controllable, and
free from the drag-and-drop editor workflows that make UI in most
game engines tedious.

The three-layer model:

| Layer | Language | Responsibility |
| --- | --- | --- |
| Structure + Logic | Iris (`.iris`) | What exists and how it behaves |
| Style + Transitions | Lustre (`.lustre`) | What it looks like and how it animates |
| Engine State | Nyx + Umbra | What is rendered and when |

---

## Design Goals

### 1. Composition First

UI is built from small, reusable components composed into larger
ones. A button, a health bar, a full menu screen — all are Iris
components of equal standing. The developer decides the granularity.

There is no distinction between a component and a page at the
language level. A `StartMenu.iris` and a `Button.iris` are both
components. Which one gets rendered is an Umbra and Nyx concern.

---

### 2. Plain Text

All UI is defined in plain text files. No editor, no drag and drop,
no XML-based scene files. Iris components are human readable,
writable in any text editor, and fully version controllable.

---

### 3. Separation of Concerns

Structure and logic live in `.iris`.
Style and transitions live in `.lustre`.

Lustre is never inline. Global styles live in a root `.lustre` file.
Component styles live in a scoped `.lustre` file alongside the
component. Component styles take precedence over global styles.

---

### 4. Dumb Components, Smart Pages

Low level components should be as simple as possible. They receive
data via props and render accordingly. They do not manage state.

Higher level compositions — pages, screens, HUDs — own state and
pass it down to child components as props. This mirrors the React
model of lifting state up.

---

### 5. Familiar to Web Developers

Iris is designed to feel natural to developers with web experience.
The JSX-style syntax, props model, and component composition pattern
are intentional borrowings from the React ecosystem.

---

## File Model

| Extension | Purpose |
| --- | --- |
| `.iris` | UI component file. Structure and logic together. |
| `.lustre` | Component-scoped or global stylesheet. |

Each component lives in its own `.iris` file. The file name matches
the component name.

```
HealthBar.iris
HealthBar.lustre
StartMenu.iris
StartMenu.lustre
Button.iris
Button.lustre
```

A global stylesheet applies baseline styles across all components.

```
global.lustre
```

---

## Component Model

An Iris component is a class defined in a `.iris` file. It returns
UI structure using JSX-style syntax.

```
component HealthBar(props: HealthBarProps) {
    render {
        <frame class="health-bar-container">
            <inline class="label">{props.label}</inline>
            <ProgressBar
                value={props.current}
                max={props.max}
                class="health-bar"
            />
        </frame>
    }
}
```

### Props

Props are the data passed into a component from its parent.

Props are defined as a struct or class and passed at the call site.

```
struct HealthBarProps {
    string label;
    f32 current;
    f32 max;
}
```

Low level components are driven entirely by props. They hold no
internal state and are purely a function of what they are given.

### State

State is data owned and managed by a component internally.

State is appropriate at the page or screen level where the component
is responsible for coordinating child components.

```
component StartMenu() {
    state {
        bool settingsOpen = false;
    }

    render {
        <frame class="start-menu">
            <Button
                label="Settings"
                onPress={() => settingsOpen = true}
            />
            if (settingsOpen) {
                <SettingsPage onClose={() => settingsOpen = false} />
            }
        </frame>
    }
}
```

### Events

Events are callbacks passed as props from parent to child.

The child component declares that an event exists. The parent
decides what happens when it fires. The child has no knowledge of
what the parent does with it.

```
component Button(props: ButtonProps) {
    render {
        <frame class="button" onPress={props.onPress}>
            <inline class="button-label">{props.label}</inline>
        </frame>
    }
}
```

Iris declares that an event exists.
Lustre decides what the button looks like when hovered or pressed.
Nyx decides what happens when it is clicked.

---

## Layout Model

Iris uses an HTML-influenced box model for layout.

### Primitives

Primitives are lowercase. Custom components are PascalCase. This
distinction is intentional — at a glance you know whether you are
looking at a primitive or a composed component.

| Element | Purpose |
| --- | --- |
| `<frame>` | General purpose block container. The primary layout primitive. Does what `<div>` does in HTML. |
| `<inline>` | Inline element. Does what `<span>` does in HTML. |
| `<grid>` | Grid-based layout. |

Everything else — direction, alignment, spacing, layering,
scrolling — is Lustre's job applied to these primitives.

```
<frame class="hud-row">
    <frame class="icon-container">
        <image src="assets/icons/health.png" />
    </frame>
    <HealthBar current={player.health} max={player.maxHealth} />
</frame>
```

Sizing supports both fixed values and relative/responsive values to
handle different screen sizes and resolutions.

---

## Importing Components

Components are imported by name.

```
import HealthBar
import Button
import SettingsPage
```

Resolves to `HealthBar.iris`, `Button.iris`, `SettingsPage.iris`
in the search path.

---

## Asset References

Assets are referenced by string path.

```
<Image src="assets/ui/logo.png" />
<Text font="assets/fonts/heading.ttf">Hello</Text>
```

Asset loading and management is an Umbra Engine concern. Iris
simply references assets by path.

---

## 3D Elements in UI

Iris supports rendering 3D objects and animations as part of UI
compositions. This enables Persona and Metaphor-style menus where
3D models are rendered and animated within the UI layer.

3D model animations are handled by Nyx. Iris provides the container
and mounting point for the 3D element.

```
component CharacterPreview(props: CharacterPreviewProps) {
    render {
        <frame class="preview-container">
            <model3d
                src={props.modelPath}
                animation={props.currentAnimation}
                class="character-model"
            />
        </frame>
    }
}
```

Lustre handles transitions around the container. Nyx drives the
animation state of the model itself.

---

## Routing

Routing is not an Iris concern.

Umbra Engine manages application state via a state machine. Nyx
decides which `.iris` component is active based on the current
engine state. Iris components have no knowledge of routing or
navigation.

```
// Nyx UIComponent deciding which screen to render
class MainUIController : UIComponent {
    void OnStateChange(GameState state) {
        match state {
            GameState.MainMenu => Render<StartMenu>(),
            GameState.InGame  => Render<HUD>(),
            GameState.Paused  => Render<PauseMenu>()
        }
    }
}
```

---

## Relationship to Nyx

Iris has Nyx DNA but is a distinct file format that parses
differently. The `.iris` extension signals to the compiler and
tooling that the file contains JSX-style UI syntax alongside Nyx
logic.

The separation is intentional. A naming convention like
`HealthBarWidget.nyx` would be ambiguous. `HealthBar.iris` is
unambiguous.

---

## Deferred Features

The following are out of scope for early versions:

- Animation system details (Lustre transition model to be defined
in the Lustre doc)
- Full layout specification
- Responsive layout breakpoints
- Accessibility
- Input focus and navigation model (controller/keyboard navigation
through UI elements)
- Iris component testing via Cimmerian

---

## Status

Iris is currently in the **design phase**.

Development will begin once Umbra Engine has matured through the
early game milestones and a real in-game UI authoring need has
emerged to drive the design forward.
