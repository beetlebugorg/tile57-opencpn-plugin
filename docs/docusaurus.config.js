// @ts-check
import {themes as prismThemes} from 'prism-react-renderer';

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'tile57 OpenCPN plugin',
  tagline: '⚓ S-57/S-101 vector charts in OpenCPN, drawn on the GPU by the tile57 engine.',

  url: 'https://beetlebugorg.github.io',
  baseUrl: '/tile57-opencpn-plugin/',

  organizationName: 'beetlebugorg',
  projectName: 'tile57-opencpn-plugin',

  onBrokenLinks: 'warn',

  markdown: {
    hooks: {
      onBrokenMarkdownLinks: 'warn',
    },
  },

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          routeBasePath: '/',
          sidebarPath: './sidebars.js',
          editUrl:
            'https://github.com/beetlebugorg/tile57-opencpn-plugin/tree/main/docs/',
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      }),
    ],
  ],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      image: 'img/annapolis-harbor.png',
      navbar: {
        title: 'tile57 OpenCPN plugin',
        items: [
          {
            href: 'https://github.com/beetlebugorg/tile57',
            label: 'tile57 engine',
            position: 'right',
          },
          {
            href: 'https://github.com/beetlebugorg/tile57-opencpn-plugin',
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              {label: 'Introduction', to: '/'},
              {label: 'Building', to: '/building'},
              {label: 'Getting Started', to: '/getting-started'},
            ],
          },
          {
            title: 'More',
            items: [
              {
                label: 'GitHub',
                href: 'https://github.com/beetlebugorg/tile57-opencpn-plugin',
              },
              {
                label: 'tile57 (the chart engine)',
                href: 'https://github.com/beetlebugorg/tile57',
              },
              {
                label: 'OpenCPN',
                href: 'https://opencpn.org',
              },
            ],
          },
        ],
        copyright: `Copyright © ${new Date().getFullYear()} Jeremy Collins.`,
      },
      prism: {
        theme: prismThemes.github,
        darkTheme: prismThemes.dracula,
        additionalLanguages: ['bash', 'json', 'cpp', 'cmake'],
      },
    }),
};

export default config;
