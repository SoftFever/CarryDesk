import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('vue-i18n', () => ({
  createI18n: vi.fn(options => options),
}))

import { createI18n } from 'vue-i18n'
import createSunshineI18n, {
  brandLocaleMessages,
  loadLocaleMessages,
  PRODUCT_NAME,
} from '../../src_assets/common/assets/web/locale.js'

describe('locale branding', () => {
  it('shows the product name in place of the capitalized upstream name', () => {
    const catalog = {
      greeting: 'Welcome to Sunshine!',
      nested: {
        possessive: "Sunshine's state file",
        suffixed: 'A Sunshine-t újraindítja',
        adjacent: '重启Sunshine后生效',
        repeated: 'Sunshine restarts Sunshine',
      },
    }
    const original = structuredClone(catalog)

    expect(brandLocaleMessages(catalog)).toEqual({
      greeting: `Welcome to ${PRODUCT_NAME}!`,
      nested: {
        possessive: `${PRODUCT_NAME}'s state file`,
        suffixed: `A ${PRODUCT_NAME}-t újraindítja`,
        adjacent: `重启${PRODUCT_NAME}后生效`,
        repeated: `${PRODUCT_NAME} restarts ${PRODUCT_NAME}`,
      },
    })
    expect(catalog).toEqual(original)
  })

  it('keeps identifiers, lowercase names and the sunny theme name', () => {
    const catalog = {
      navbar: { theme_sunshine: 'Sunshine', other: 'Sunshine' },
      paths: 'Edit sunshine.conf as the sunshine user',
      identifier: 'SunshineVersion and MySunshine',
    }

    expect(brandLocaleMessages(catalog)).toEqual({
      navbar: { theme_sunshine: 'Sunshine', other: PRODUCT_NAME },
      paths: 'Edit sunshine.conf as the sunshine user',
      identifier: 'SunshineVersion and MySunshine',
    })
  })
})

describe('locale initialization', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
  })

  beforeEach(() => {
    createI18n.mockClear()
    document.documentElement.removeAttribute('lang')
  })

  it('loads a bundled configured locale', async () => {
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce({ json: async () => ({ locale: 'de' }) })
      .mockResolvedValueOnce({ json: async () => ({ greeting: 'Hallo' }) }))

    const i18n = await createSunshineI18n()

    expect(fetch).toHaveBeenCalledWith('./api/configLocale')
    expect(fetch).toHaveBeenCalledWith('./assets/locale/de.json')
    expect(document.documentElement.lang).toBe('de')
    expect(i18n.locale).toBe('de')
    expect(i18n.fallbackLocale).toBe('en')
    expect(i18n.messages.en).toBeDefined()
    expect(i18n.messages.de).toEqual({ greeting: 'Hallo' })
  })

  it('brands the English fallback and the loaded translation', async () => {
    const result = await loadLocaleMessages('de', async () => ({
      welcome: { greeting: 'Willkommen bei Sunshine!' },
    }))

    expect(result.messages.de.welcome.greeting).toBe(`Willkommen bei ${PRODUCT_NAME}!`)
    expect(result.messages.en.welcome.greeting).toBe(`Welcome to ${PRODUCT_NAME}!`)
    expect(result.messages.en.config.sunshine_name).toBe(`${PRODUCT_NAME} Name`)
    expect(result.messages.en.navbar.theme_sunshine).toBe('Sunshine')
  })

  it('brands the English messages when English is configured', async () => {
    const result = await loadLocaleMessages('en')

    expect(result.messages.en.welcome.greeting).toBe(`Welcome to ${PRODUCT_NAME}!`)
  })

  it.each([
    ['an unsupported locale', 'unsupported'],
    ['a prototype property', '__proto__'],
    ['a non-string value', null],
  ])('falls back to English for %s', async (_description, locale) => {
    vi.stubGlobal('fetch', vi.fn(async () => ({
      json: async () => ({ locale }),
    })))

    const i18n = await createSunshineI18n()

    expect(document.documentElement.lang).toBe('en')
    expect(i18n.locale).toBe('en')
    expect(Object.keys(i18n.messages)).toEqual(['en'])
  })

  it('falls back to English when a bundled translation fails to load', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => {})
    const loadTranslation = async () => {
      throw new Error('translation unavailable')
    }

    const result = await loadLocaleMessages('de', loadTranslation)

    expect(result.locale).toBe('en')
    expect(Object.keys(result.messages)).toEqual(['en'])
    expect(error).toHaveBeenCalledWith('Failed to download translations', expect.any(Error))
    error.mockRestore()
  })

  it('omits malformed messages while preserving valid translations and interpolation syntax', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => {})
    const catalog = {
      broken: 'Loadingchar@@0',
      nested: {
        broken: 'Hello {name',
        greeting: 'Hallo {name}',
        list: '{0}',
        literal: "{'@'}",
        linked: '@:nested.greeting',
        plural: 'none | one | {count} items',
        empty: '',
      },
    }
    const original = structuredClone(catalog)

    const result = await loadLocaleMessages('de', async () => catalog)

    expect(result.locale).toBe('de')
    expect(result.messages.de).toEqual({
      nested: {
        greeting: 'Hallo {name}',
        list: '{0}',
        literal: "{'@'}",
        linked: '@:nested.greeting',
        plural: 'none | one | {count} items',
        empty: '',
      },
    })
    expect(result.messages.en).toBeDefined()
    expect(catalog).toEqual(original)
    expect(error).toHaveBeenCalledTimes(2)
    expect(error).toHaveBeenCalledWith('Ignoring invalid translation', 'de', 'broken', expect.any(SyntaxError))
    expect(error).toHaveBeenCalledWith('Ignoring invalid translation', 'de', 'nested.broken', expect.any(SyntaxError))
  })
})
