/* ============================================
   OpenChess Build Guide — JSON-driven Builder + Interactivity
   ============================================ */

(async function () {
    'use strict';

    // ---------- Load content.json ----------
    const resp = await fetch('content.json');
    const data = await resp.json();
    const IMG = data.imagePath || '../BuildGuide/';
    const GITHUB = data.githubUrl || '#';

    const contentEl = document.getElementById('content');
    const navUl = document.getElementById('nav-links');

    // ---------- Helper: create element shorthand ----------
    function el(tag, attrs, ...children) {
        const e = document.createElement(tag);
        if (attrs) Object.entries(attrs).forEach(([k, v]) => {
            if (k === 'className') e.className = v;
            else if (k === 'innerHTML') e.innerHTML = v;
            else if (k === 'style' && typeof v === 'object') Object.assign(e.style, v);
            else e.setAttribute(k, v);
        });
        children.forEach(c => {
            if (c == null) return;
            e.append(typeof c === 'string' ? document.createTextNode(c) : c);
        });
        return e;
    }

    // ---------- Helper: build image container(s) ----------
    function buildImages(images) {
        if (images.length === 1) {
            return buildSingleImage(images[0]);
        }
        const row = el('div', { className: 'image-row' });
        images.forEach(img => row.append(buildSingleImage(img)));
        return row;
    }

    function buildSingleImage(imgData) {
        const container = el('div', { className: 'image-container' });
        const attrs = {
            src: IMG + encodeURI(imgData.file),
            alt: imgData.alt || '',
            loading: 'lazy'
        };
        if (!imgData.noZoom) attrs.className = 'zoomable';
        if (imgData.maxWidth) attrs.style = { maxWidth: imgData.maxWidth };
        container.append(el('img', attrs));
        if (imgData.caption) {
            container.append(el('span', { className: 'image-caption' }, imgData.caption));
        }
        return container;
    }

    // ---------- Add nav link ----------
    function addNavLink(href, label, isFirst) {
        const li = el('li');
        const a = el('a', { href: '#' + href }, label);
        if (isFirst) a.classList.add('active');
        li.append(a);
        navUl.append(li);
    }

    // ---------- Build: Hero ----------
    function buildHero(hero) {
        const section = el('section', { id: 'hero', className: 'hero-section' });

        const textDiv = el('div', { className: 'hero-text' });
        textDiv.append(
            el('h1', { innerHTML: hero.title + '<br><span class="accent">' + hero.titleAccent + '</span>' }),
            el('p', { className: 'hero-description' }, hero.description),
            el('a', { href: '#materials', className: 'btn-primary' }, 'Get Started →'),
            el('a', { href: GITHUB, className: 'btn-secondary', target: '_blank' }, 'View on GitHub')
        );

        const imgDiv = el('div', { className: 'hero-image' });
        imgDiv.append(el('img', { src: IMG + hero.image, alt: 'OpenChess demo', loading: 'lazy' }));

        section.append(textDiv, imgDiv);
        contentEl.append(section);
        addNavLink('hero', 'Overview', true);
    }

    // ---------- Build: Materials ----------
    function buildMaterials(mat) {
        const section = el('section', { id: 'materials', className: 'section' });
        section.append(el('h2', { className: 'section-title' }, 'Materials Required'));

        const grid = el('div', { className: 'materials-grid' });
        mat.items.forEach(item => {
            grid.append(
                el('div', { className: 'material-card' },
                    el('div', { className: 'material-icon' }, item.icon),
                    el('h3', null, item.name),
                    el('p', null, item.description)
                )
            );
        });
        section.append(grid);

        section.append(el('div', { className: 'info-box', innerHTML: '<strong>Tools needed:</strong> ' + mat.toolsNote }));

        contentEl.append(section);
        addNavLink('materials', mat.navLabel);
    }

    // ---------- Build: Step sections ----------
    function buildStepSection(sec) {
        const section = el('section', { id: sec.id, className: 'section' });
        section.append(el('h2', { className: 'section-title' }, sec.title));
        if (sec.intro) section.append(el('p', { className: 'section-intro' }, sec.intro));

        (sec.steps || []).forEach(step => {
            const card = el('div', { className: 'step-card' });

            if (step.centerContent) card.style.textAlign = 'center';
            if (step.title) card.append(el('h3', null, step.title));
            if (step.description) card.append(el('p', null, step.description));

            if (step.images && step.images.length) {
                // Demo gallery: split into rows of 2
                if (sec.id === 'demo' && step.images.length > 2 && !step.centerContent) {
                    for (let i = 0; i < step.images.length; i += 2) {
                        const row = el('div', { className: 'image-row' });
                        if (i > 0) row.style.marginTop = '1.5rem';
                        row.append(buildSingleImage(step.images[i]));
                        if (step.images[i + 1]) row.append(buildSingleImage(step.images[i + 1]));
                        card.append(row);
                    }
                } else {
                    card.append(buildImages(step.images));
                }
            }

            section.append(card);
        });

        contentEl.append(section);
        addNavLink(sec.id, sec.navLabel);
    }

    // ---------- Build: Software ----------
    function buildSoftware(sw) {
        const section = el('section', { id: 'software', className: 'section' });
        section.append(el('h2', { className: 'section-title' }, sw.title));
        if (sw.intro) section.append(el('p', { className: 'section-intro' }, sw.intro));

        // Web Installer callout
        if (sw.webInstaller) {
            const installerBox = el('div', { className: 'info-box', style: { marginBottom: '1.5rem' } });
            installerBox.innerHTML = '<strong>⚡ Quick Flash</strong> ' + sw.webInstaller.text;
            const btn = el('a', {
                href: sw.webInstaller.buttonUrl,
                className: 'btn-primary',
                style: { display: 'inline-block', marginTop: '12px', fontSize: '0.95rem', padding: '10px 24px' }
            }, sw.webInstaller.buttonText);
            installerBox.append(btn);
            section.append(installerBox);
        }

        const card = el('div', { className: 'step-card software-steps' });
        sw.steps.forEach(step => {
            card.append(el('h3', null, step.title));
            const ul = el('ul');
            step.items.forEach(item => ul.append(el('li', { innerHTML: item })));
            card.append(ul);
        });
        section.append(card);

        if (sw.helpNote) {
            section.append(el('div', { className: 'info-box', innerHTML: '<strong>Need help?</strong> ' + sw.helpNote }));
        }

        contentEl.append(section);
        addNavLink('software', sw.navLabel);
    }

    // ---------- Build: Footer ----------
    function buildFooter() {
        const footer = el('footer');
        footer.append(el('p', { innerHTML: 'OpenChess Build Guide — <a href="' + GITHUB + '" target="_blank">View on GitHub</a>' }));
        contentEl.append(footer);
    }

    // =====================
    //  Assemble the page
    // =====================
    buildHero(data.hero);
    buildMaterials(data.materials);
    data.sections.forEach(sec => buildStepSection(sec));
    buildSoftware(data.software);
    buildFooter();

    // ====================================================
    //  Interactive features (init after DOM is built)
    // ====================================================

    // --- Sidebar active link tracking ---
    (function () {
        const sections = document.querySelectorAll('section[id]');
        const navLinks = document.querySelectorAll('#nav-links a');

        function updateActiveLink() {
            const scrollY = window.scrollY + 120;
            let currentId = '';
            sections.forEach(s => { if (s.offsetTop <= scrollY) currentId = s.id; });
            navLinks.forEach(link => {
                link.classList.toggle('active', link.getAttribute('href') === '#' + currentId);
            });
        }

        window.addEventListener('scroll', updateActiveLink, { passive: true });
        updateActiveLink();
    })();

    // --- Mobile sidebar toggle ---
    (function () {
        const sidebar = document.getElementById('sidebar');
        const toggleBtn = document.getElementById('menu-toggle');
        const navLinks = document.querySelectorAll('#nav-links a');

        if (toggleBtn) {
            toggleBtn.addEventListener('click', () => sidebar.classList.toggle('open'));
        }

        navLinks.forEach(link => {
            link.addEventListener('click', () => {
                if (window.innerWidth <= 768) sidebar.classList.remove('open');
            });
        });

        contentEl.addEventListener('click', () => {
            if (window.innerWidth <= 768 && sidebar.classList.contains('open')) {
                sidebar.classList.remove('open');
            }
        });
    })();

    // --- Image zoom modal ---
    (function () {
        const modal = document.getElementById('zoom-modal');
        const modalImg = document.getElementById('zoom-img');
        const closeBtn = document.querySelector('.zoom-close');
        const prevBtn = document.getElementById('zoom-prev');
        const nextBtn = document.getElementById('zoom-next');
        const counter = document.getElementById('zoom-counter');
        const zoomables = Array.from(document.querySelectorAll('.zoomable'));

        let currentIndex = 0;
        let scale = 1, translateX = 0, translateY = 0;
        let isDragging = false, dragStartX = 0, dragStartY = 0, dragStartTX = 0, dragStartTY = 0;
        const MIN_SCALE = 0.5, MAX_SCALE = 8;

        function applyTransform() {
            modalImg.style.transform = `translate(${translateX}px, ${translateY}px) scale(${scale})`;
            modalImg.classList.toggle('zoomed', scale > 1);
        }

        function resetTransform() {
            scale = 1; translateX = 0; translateY = 0;
            applyTransform();
        }

        function updateCounter() { counter.textContent = `${currentIndex + 1} / ${zoomables.length}`; }

        function updateNavButtons() {
            prevBtn.classList.toggle('disabled', currentIndex === 0);
            nextBtn.classList.toggle('disabled', currentIndex === zoomables.length - 1);
        }

        function showImage(index) {
            currentIndex = index;
            modalImg.src = zoomables[index].src;
            modalImg.alt = zoomables[index].alt;
            resetTransform();
            updateCounter();
            updateNavButtons();
        }

        function goNext() { if (currentIndex < zoomables.length - 1) showImage(currentIndex + 1); }
        function goPrev() { if (currentIndex > 0) showImage(currentIndex - 1); }

        zoomables.forEach((img, idx) => {
            img.addEventListener('click', () => {
                modal.classList.add('active');
                document.body.style.overflow = 'hidden';
                showImage(idx);
            });
        });

        function closeModal() {
            modal.classList.remove('active');
            document.body.style.overflow = '';
            resetTransform();
        }

        if (closeBtn) closeBtn.addEventListener('click', closeModal);
        prevBtn.addEventListener('click', (e) => { e.stopPropagation(); goPrev(); });
        nextBtn.addEventListener('click', (e) => { e.stopPropagation(); goNext(); });

        modal.addEventListener('click', (e) => { if (e.target === modal) closeModal(); });

        document.addEventListener('keydown', (e) => {
            if (!modal.classList.contains('active')) return;
            switch (e.key) {
                case 'Escape': closeModal(); break;
                case 'ArrowLeft': e.preventDefault(); goPrev(); break;
                case 'ArrowRight': e.preventDefault(); goNext(); break;
            }
        });

        // Scroll wheel zoom
        modal.addEventListener('wheel', (e) => {
            if (!modal.classList.contains('active')) return;
            e.preventDefault();
            const rect = modalImg.getBoundingClientRect();
            const imgCX = rect.left + rect.width / 2, imgCY = rect.top + rect.height / 2;
            const px = e.clientX - imgCX, py = e.clientY - imgCY;
            const prev = scale;
            scale = Math.min(MAX_SCALE, Math.max(MIN_SCALE, scale * (e.deltaY > 0 ? 0.9 : 1.1)));
            const ratio = 1 - scale / prev;
            translateX += px * ratio; translateY += py * ratio;
            applyTransform();
        }, { passive: false });

        // Pinch to zoom
        let lastTouchDist = 0, lastTouchCenter = { x: 0, y: 0 };
        function touchDist(t) { const dx = t[0].clientX - t[1].clientX, dy = t[0].clientY - t[1].clientY; return Math.sqrt(dx * dx + dy * dy); }
        function touchCenter(t) { return { x: (t[0].clientX + t[1].clientX) / 2, y: (t[0].clientY + t[1].clientY) / 2 }; }

        modal.addEventListener('touchstart', (e) => {
            if (!modal.classList.contains('active')) return;
            if (e.touches.length === 2) {
                e.preventDefault();
                lastTouchDist = touchDist(e.touches);
                lastTouchCenter = touchCenter(e.touches);
            } else if (e.touches.length === 1 && scale > 1) {
                isDragging = true;
                dragStartX = e.touches[0].clientX; dragStartY = e.touches[0].clientY;
                dragStartTX = translateX; dragStartTY = translateY;
            }
        }, { passive: false });

        modal.addEventListener('touchmove', (e) => {
            if (!modal.classList.contains('active')) return;
            if (e.touches.length === 2) {
                e.preventDefault();
                const dist = touchDist(e.touches), center = touchCenter(e.touches);
                const rect = modalImg.getBoundingClientRect();
                const imgCX = rect.left + rect.width / 2, imgCY = rect.top + rect.height / 2;
                const px = center.x - imgCX, py = center.y - imgCY;
                const prev = scale;
                scale = Math.min(MAX_SCALE, Math.max(MIN_SCALE, scale * (dist / lastTouchDist)));
                const ratio = 1 - scale / prev;
                translateX += px * ratio + center.x - lastTouchCenter.x;
                translateY += py * ratio + center.y - lastTouchCenter.y;
                lastTouchDist = dist; lastTouchCenter = center;
                applyTransform();
            } else if (e.touches.length === 1 && isDragging) {
                e.preventDefault();
                translateX = dragStartTX + (e.touches[0].clientX - dragStartX);
                translateY = dragStartTY + (e.touches[0].clientY - dragStartY);
                applyTransform();
            }
        }, { passive: false });

        modal.addEventListener('touchend', (e) => {
            if (e.touches.length < 2) lastTouchDist = 0;
            if (e.touches.length === 0) isDragging = false;
        });

        // Mouse drag when zoomed
        modalImg.addEventListener('mousedown', (e) => {
            if (scale <= 1) return;
            e.preventDefault();
            isDragging = true;
            dragStartX = e.clientX; dragStartY = e.clientY;
            dragStartTX = translateX; dragStartTY = translateY;
            modalImg.classList.add('dragging');
        });
        window.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            translateX = dragStartTX + (e.clientX - dragStartX);
            translateY = dragStartTY + (e.clientY - dragStartY);
            applyTransform();
        });
        window.addEventListener('mouseup', () => { isDragging = false; modalImg.classList.remove('dragging'); });

        // Double-click to toggle zoom
        modalImg.addEventListener('dblclick', (e) => {
            e.stopPropagation();
            if (scale !== 1) {
                resetTransform();
            } else {
                scale = 3;
                const rect = modalImg.getBoundingClientRect();
                translateX = (rect.left + rect.width / 2 - e.clientX) * 2;
                translateY = (rect.top + rect.height / 2 - e.clientY) * 2;
                applyTransform();
            }
        });
    })();

    // --- Smooth reveal on scroll ---
    (function () {
        const cards = document.querySelectorAll('.step-card, .material-card');
        const observer = new IntersectionObserver((entries) => {
            entries.forEach(entry => {
                if (entry.isIntersecting) {
                    entry.target.style.opacity = '1';
                    entry.target.style.transform = 'translateY(0)';
                    observer.unobserve(entry.target);
                }
            });
        }, { threshold: 0.1, rootMargin: '0px 0px -40px 0px' });

        cards.forEach(card => {
            card.style.opacity = '0';
            card.style.transform = 'translateY(20px)';
            card.style.transition = 'opacity 0.5s ease, transform 0.5s ease';
            observer.observe(card);
        });
    })();

    // --- Floating step navigator ---
    (function () {
        const stepCards = Array.from(document.querySelectorAll('.step-card'));
        const nav = document.getElementById('step-nav');
        const prevBtn = document.getElementById('step-prev');
        const nextBtn = document.getElementById('step-next');
        const counterEl = document.getElementById('step-counter');
        const total = stepCards.length;

        if (!total) { nav.classList.add('hidden'); return; }

        function getScrollPad() {
            return parseFloat(getComputedStyle(document.documentElement).scrollPaddingTop) || 80;
        }

        function findCurrentStep() {
            const scrollY = window.scrollY + getScrollPad() + 40;
            let idx = -1;
            for (let i = 0; i < total; i++) {
                if (stepCards[i].offsetTop <= scrollY) idx = i;
            }
            return idx;
        }

        function updateNav() {
            const idx = findCurrentStep();
            counterEl.textContent = `${Math.max(0, idx) + 1} / ${total}`;
            prevBtn.classList.toggle('disabled', idx <= 0);
            nextBtn.classList.toggle('disabled', idx >= total - 1);
            const firstTop = stepCards[0].offsetTop - window.innerHeight;
            const lastBottom = stepCards[total - 1].offsetTop + stepCards[total - 1].offsetHeight;
            nav.classList.toggle('hidden', !(window.scrollY >= firstTop && window.scrollY <= lastBottom));
        }

        function scrollToStep(idx) {
            if (idx >= 0 && idx < total) stepCards[idx].scrollIntoView({ behavior: 'smooth', block: 'start' });
        }

        prevBtn.addEventListener('click', () => scrollToStep(Math.max(0, findCurrentStep() - 1)));
        nextBtn.addEventListener('click', () => scrollToStep(Math.min(total - 1, findCurrentStep() + 1)));

        document.addEventListener('keydown', (e) => {
            const modal = document.getElementById('zoom-modal');
            if (modal && modal.classList.contains('active')) return;
            const dir = (e.key === 'ArrowUp' || e.key === 'ArrowLeft') ? -1
                : (e.key === 'ArrowDown' || e.key === 'ArrowRight') ? 1 : 0;
            if (!dir) return;
            e.preventDefault();
            scrollToStep(findCurrentStep() + dir);
        });

        window.addEventListener('scroll', updateNav, { passive: true });
        window.addEventListener('resize', updateNav, { passive: true });
        updateNav();
    })();

})();
